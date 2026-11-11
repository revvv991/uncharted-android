// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64 (issue #16: "Terios X3 gamepad not working ... It works in all other emulators"):
// controller setups made in the app's Controller setup window, applied to SDL.
//
// The port reads SDL gamepads only (runtime_pad.c, SDL_GetGamepads). On Android SDL makes a
// gamepad of a joystick by itself only when the device reports Android's keys for the face
// buttons (SDL_CreateMappingForAndroidGamepad); a pad in a "PC" or DirectInput mode reports
// generic buttons (KEYCODE_BUTTON_1..16) and was not read. The app's Controller setup window
// (the launcher's button and the in-game panel's item: ControllerSetup.java) draws the on-screen
// gamepad; the player gives its controls the controller's inputs, in SDL's numbers
// (PadMapping.java), and the window writes them to files/controllers.txt, a line per controller:
//     vendor:product:crc TAB name TAB a:b20,b:b21,...
// A joystick whose GUID carries the same vendor, product and CRC (of its Android descriptor: one
// physical controller) gets those fields as an SDL mapping under its own GUID, when it connects
// and again when the file changes. SDL keeps the CRC with the mapping, so it wins over SDL's own
// mapping of the device: a pad SDL maps already can be set up too.
//
// The window over the game takes the focus, which pauses SDL (the background's path: presenting
// and the audio stop, the window thread blocks in SDL's event pump) until it closes; the file is
// looked at once a second.
//
// Threads: all but RequestSetup on the window thread (Event from the overlay's HandleEvent there,
// under its ImGui lock); `mutex` is taken before SDL's joystick lock.

#include "bbport_padmap.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include <jni.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

namespace BbPadMap {

namespace {

constexpr Uint64 NoticeEveryMs = 30000;
constexpr Uint64 CheckEveryMs = 1000;

struct Setup {
    std::string name, fields;
};

std::mutex mutex;
std::string file_path;                                        // controllers.txt
std::map<std::string, Setup> setups;                          // by vendor:product:crc
bool file_seen = false;                                       // what Read found
struct timespec file_time {};
off_t file_size = 0;
std::map<SDL_JoystickID, std::string> applied;                // joysticks given a setup: its fields
std::map<std::string, std::optional<std::string>> originals;  // GUID: SDL's mapping before ours
std::map<SDL_JoystickID, SDL_Joystick*> unmapped;             // opened to hear them
Uint64 last_notice_ms = 0, last_check_ms = 0;
std::atomic<bool> setup_requested{false};

std::string GuidOf(SDL_JoystickID id) {
    char text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), text, int(sizeof(text)));
    return text;
}

// Also the mapping's name field: no commas (they separate the fields).
std::string NameOf(SDL_JoystickID id) {
    const char* raw = SDL_GetJoystickNameForID(id);
    std::string name = raw && *raw ? raw : "Controller";
    std::replace(name.begin(), name.end(), ',', ' ');
    return name;
}

/// vendor:product:crc of the joystick's GUID (PadMapping.key makes the same from the device).
std::string KeyOf(SDL_JoystickID id) {
    Uint16 vendor = 0, product = 0, version = 0, crc = 0;
    SDL_GetJoystickGUIDInfo(SDL_GetJoystickGUIDForID(id), &vendor, &product, &version, &crc);
    char key[16];
    std::snprintf(key, sizeof(key), "%04x:%04x:%04x", unsigned(vendor), unsigned(product), unsigned(crc));
    return key;
}

bool ValidKey(const std::string& key) {
    if (key.size() != 14 || key[4] != ':' || key[9] != ':') {
        return false;
    }
    for (size_t i = 0; i < key.size(); ++i) {
        if (i != 4 && i != 9 && !std::strchr("0123456789abcdef", key[i])) {
            return false;
        }
    }
    return true;
}

/// The mapping's fields as the window writes them: fields of letters, digits and + - ~ : ,
bool ValidFields(const std::string& fields) {
    return fields.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789:,+-~") == std::string::npos;
}

void Read() {
    setups.clear();
    struct stat st {};
    file_seen = ::stat(file_path.c_str(), &st) == 0;
    file_time = file_seen ? st.st_mtim : timespec{};
    file_size = file_seen ? st.st_size : 0;
    FILE* file = file_seen ? std::fopen(file_path.c_str(), "r") : nullptr;
    if (!file) {
        return;
    }
    char buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), file)) {
        std::string line = buffer;
        line.erase(line.find_last_not_of("\r\n") + 1);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t tab1 = line.find('\t'), tab2 = tab1 == std::string::npos ? tab1 : line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, tab1), fields = line.substr(tab2 + 1);
        if (ValidKey(key) && ValidFields(fields)) {
            setups[key] = Setup{line.substr(tab1 + 1, tab2 - tab1 - 1), fields};
        }
    }
    std::fclose(file);
}

bool FileChanged() {
    struct stat st {};
    const bool seen = ::stat(file_path.c_str(), &st) == 0;
    if (seen != file_seen) {
        return true;
    }
    return seen && (st.st_size != file_size || st.st_mtim.tv_sec != file_time.tv_sec ||
                    st.st_mtim.tv_nsec != file_time.tv_nsec);
}

/// The joystick's setup applied; SDL's own mapping back when the file has none for it any more.
void Apply(SDL_JoystickID id) {
    const std::string key = KeyOf(id), guid = GuidOf(id), name = NameOf(id);
    const auto setup = setups.find(key);
    const auto was = applied.find(id);
    if (setup != setups.end()) {
        const std::string& fields = setup->second.fields;
        if (was != applied.end() && was->second == fields) {
            return;
        }
        if (!originals.count(guid)) {
            char* mapping = SDL_GetGamepadMappingForID(id);
            originals[guid] = mapping ? std::optional<std::string>(mapping) : std::nullopt;
            SDL_free(mapping);
        }
        // SDL takes a mapping file's line only with its platform's field; one added by hand needs
        // none, but the same text is kept for both.
        const std::string line = guid + "," + name + "," + fields + (fields.empty() ? "" : ",") +
                                 "platform:" + SDL_GetPlatform() + ",";
        if (SDL_AddGamepadMapping(line.c_str()) < 0) {
            std::printf("Input: the controller setup of \"%s\" refused by SDL: %s (%s)\n",
                        name.c_str(), SDL_GetError(), line.c_str());
            std::fflush(stdout);
            return;
        }
        applied[id] = fields;
        std::printf("Input: controller setup applied to \"%s\" (%s): %s\n", name.c_str(),
                    key.c_str(), fields.c_str());
        std::fflush(stdout);
        // A gamepad now: the game opens it itself.
        if (const auto it = unmapped.find(id); it != unmapped.end()) {
            SDL_CloseJoystick(it->second);
            unmapped.erase(it);
        }
    } else if (was != applied.end()) {
        applied.erase(was);
        const auto original = originals.find(guid);
        const bool back = original != originals.end() && original->second &&
                          SDL_AddGamepadMapping(original->second->c_str()) >= 0;
        std::printf("Input: controller setup of \"%s\" removed (%s)\n", name.c_str(),
                    back ? "SDL's own mapping is back"
                         : "SDL has no mapping of its own for it: the setup stays until the game starts again");
        std::fflush(stdout);
    }
}

std::string NotSetUp(SDL_JoystickID id) {
    return "\"" + NameOf(id) +
           "\" is not set up, so the game does not read it. Swipe in from the right edge: "
           "Controller setup.";
}

/// GameActivity.openControllerSetup: the window over the game (on the UI thread).
void OpenSetup() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    bool opened = false;
    if (env && activity) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID open = cls ? env->GetMethodID(cls, "openControllerSetup", "()V") : nullptr;
        if (open) {
            env->CallVoidMethod(activity, open);
            opened = !env->ExceptionCheck();
        }
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        if (cls) {
            env->DeleteLocalRef(cls);
        }
    }
    if (env && activity) {
        env->DeleteLocalRef(activity);
    }
    std::printf("Input: %s\n", opened ? "the controller setup window opened over the game"
                                      : "GameActivity.openControllerSetup failed");
    std::fflush(stdout);
}

} // namespace

void Init() {
    const char* config = std::getenv("BB_CONFIG");
    const std::string settings = config && *config ? config : "bbport.ini";
    const size_t slash = settings.find_last_of('/');
    std::scoped_lock lock{mutex};
    file_path = (slash == std::string::npos ? std::string() : settings.substr(0, slash + 1)) +
                "controllers.txt";
    Read();
    if (!setups.empty()) {
        std::printf("Input: %zu controller setup(s) in %s\n", setups.size(), file_path.c_str());
        std::fflush(stdout);
    }
}

std::string Added(SDL_JoystickID id) {
    std::scoped_lock lock{mutex};
    Apply(id);
    if (SDL_IsGamepad(id) || unmapped.count(id)) {
        return {};
    }
    // Opened, so its presses reach Event (SDL sends a joystick's events only while it is open).
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (!joystick) {
        return {};
    }
    unmapped[id] = joystick;
    last_notice_ms = SDL_GetTicks();
    return NotSetUp(id);
}

void Removed(SDL_JoystickID id) {
    std::scoped_lock lock{mutex};
    if (const auto it = unmapped.find(id); it != unmapped.end()) {
        SDL_CloseJoystick(it->second);
        unmapped.erase(it);
    }
    applied.erase(id); // connected again, it gets a new id and its setup again
}

std::string Event(const SDL_Event& event) {
    if (event.type != SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
        return {};
    }
    const SDL_JoystickID which = event.jbutton.which;
    std::scoped_lock lock{mutex};
    if (!unmapped.count(which) || SDL_IsGamepad(which)) {
        return {};
    }
    const Uint64 now = SDL_GetTicks();
    if (now - last_notice_ms < NoticeEveryMs) {
        return {};
    }
    last_notice_ms = now;
    return NotSetUp(which);
}

void Poll() {
    if (setup_requested.exchange(false)) {
        OpenSetup();
    }
    const Uint64 now = SDL_GetTicks();
    if (now - last_check_ms < CheckEveryMs) {
        return;
    }
    last_check_ms = now;
    std::scoped_lock lock{mutex};
    if (!FileChanged()) {
        return;
    }
    Read();
    std::printf("Input: %s changed: %zu controller setup(s)\n", file_path.c_str(), setups.size());
    std::fflush(stdout);
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; ++i) {
        Apply(ids[i]);
    }
    SDL_free(ids);
}

void RequestSetup() {
    setup_requested = true;
}

} // namespace BbPadMap
