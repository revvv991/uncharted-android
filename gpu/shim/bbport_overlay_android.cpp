// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64: the in-game menu for touch screens. The PC menu (bbport_overlay.cpp) is a floating
// window of combo boxes sized for a mouse; here the same BbSettings values are edited in a panel
// on the right edge of the screen with rows a finger can hit, choices as rows of buttons, and
// only what applies on Android (no DLSS, FSR 4 or XeSS). Opened with a swipe in from the right
// edge, L3+R3 or Insert; closed with a tap outside it, the B button or its Close button.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <type_traits>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "bbport_overlay_android.h"
#include "bbport_padmap.h"
#include "bbport_settings.h"
#include "core/emulator_settings.h"

extern "C" void runtime_restart(void);

namespace BbOverlay {

namespace {

constexpr float TouchScale = 1.8f;   // on top of the display scale: 32 px text at 1080p
constexpr float PanelShare = 0.45f;  // of the screen width

std::atomic<float> panel_left_norm{1.0f}; // the panel's left edge, 0..1 of the screen width
std::atomic<float> pending_scroll{0.0f};  // a finger's travel inside the panel, 0..1 of the height

// A change is written to bbport.ini a second after the last one, or when the panel closes
// (BbSettings::Save on every tap would rewrite the file while a slider is dragged).
bool dirty = false;
double last_change = 0.0;

// Quit game asks once more in place (a stray tap loses nothing); Cancel gets the controller's focus.
bool confirm_quit = false;
bool focus_cancel = false;

void Changed() {
    dirty = true;
    last_change = ImGui::GetTime();
}

template <typename T>
void Store(std::atomic<T>& target, std::type_identity_t<T> value) {
    if (target.load() != value) {
        target.store(value);
        Changed();
    }
}

/// A row of equal buttons, the chosen one lit. Returns true when the choice changed.
bool Segmented(const char* id, int& value, const char* const* labels, int count,
               const bool* enabled = nullptr) {
    bool changed = false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width =
        (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * float(count - 1)) / float(count);
    ImGui::PushID(id);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.85f);
    for (int i = 0; i < count; ++i) {
        if (i) {
            ImGui::SameLine();
        }
        const bool selected = value == i;
        const bool usable = !enabled || enabled[i];
        ImGui::BeginDisabled(!usable);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_ButtonActive]);
        }
        if (ImGui::Button(labels[i], ImVec2(width, 0.0f)) && !selected) {
            value = i;
            changed = true;
        }
        if (selected) {
            ImGui::PopStyleColor(2);
        }
        ImGui::EndDisabled();
    }
    ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

void Section(const char* title) {
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

void Toggle(const char* label, std::atomic<bool>& value) {
    bool v = value.load();
    if (ImGui::Checkbox(label, &v)) {
        value.store(v);
        Changed();
    }
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value.load();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat(label, &v, lo, hi, "%.2f")) {
        value.store(v);
        Changed();
    }
}

void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace

void AndroidMenu(float base_scale, float frame_ms, bool& keep_open) {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = base_scale * TouchScale;
    const float width = std::floor(viewport->WorkSize.x * PanelShare);
    panel_left_norm.store((viewport->WorkSize.x - width) / viewport->WorkSize.x);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width,
                                   viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 11.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 8.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 16.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.09f, 0.94f));
    ImGui::PushFont(nullptr, 18.0f * scale);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings;
    ImGui::GetIO().MouseDrawCursor = false;
    if (ImGui::Begin("##android_menu", nullptr, flags)) {
        if (ImGui::IsWindowAppearing()) {
            confirm_quit = false;
        }
        if (const float travel = pending_scroll.exchange(0.0f); travel != 0.0f) {
            ImGui::SetScrollY(ImGui::GetScrollY() - travel * viewport->WorkSize.y);
        }
        ImGui::Text("%.0f FPS  %.1f ms", frame_ms > 0.0f ? 1000.0f / frame_ms : 0.0f, frame_ms);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
        Toggle("FPS counter", s.show_fps);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.8f);
        if (ImGui::Button("Close##top")) {
            keep_open = false;
        }
        // Uncharted renders at the size its resolution patch sets at start (the launcher's
        // choice; the game's display buffers carry it). The port runs Uncharted in velocity
        // mode: FSR 3.1 and TAA are temporal anti-aliasing at that size (the upscaler writes
        // the game's own targets) and the picture is scaled to the screen afterwards; the PC
        // menu's quality presets (Quality, Balanced, ...) change nothing here.
        int scene_w = s.active_render_width.load(), scene_h = s.active_render_height.load();
        const bool seen = scene_w > 0; // the upscaler has met the scene
        if (!seen) {
            scene_w = s.game_display_width.load();
            scene_h = s.game_display_height.load();
        }
        const int out_w = BbSettings::OutputWidths[s.output_res];
        const int out_h = BbSettings::OutputHeights[s.output_res];
        const bool scene_is_output = scene_w == out_w && scene_h == out_h;
        ImGui::TextDisabled("Game rendered at %d x %d%s", scene_w, scene_h,
                            seen ? "" : " (its display size; no 3D scene met yet)");
        ImGui::TextDisabled("Shown at %d x %d", out_w, out_h);

        Section("Upscaling");
        static const char* upscalers[] = {"Off", "FSR 3.1", "TAA"};
        static const int upscaler_values[] = {BbSettings::UpscalerOff, BbSettings::UpscalerFsr3,
                                              BbSettings::UpscalerTaa};
        int upscaler = 0;
        for (int i = 0; i < 3; ++i) {
            if (upscaler_values[i] == s.upscaler.load()) {
                upscaler = i;
            }
        }
        if (Segmented("upscaler", upscaler, upscalers, 3)) {
            Store(s.upscaler, upscaler_values[upscaler]);
        }
        if (s.upscaler == BbSettings::UpscalerOff) {
            Note("Off: the game's own anti-aliasing");
        } else {
            ImGui::TextDisabled("%s: temporal anti-aliasing at %d x %d",
                                s.upscaler == BbSettings::UpscalerTaa ? "TAA" : "FSR 3.1",
                                scene_w, scene_h);
        }
        if (const char* problem = s.upscaler_problem.load();
            problem && s.upscaler == BbSettings::UpscalerFsr3) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", problem);
            ImGui::PopStyleColor();
        }
        if (!scene_is_output) {
            Note("The picture is scaled to the screen by the port; upscaling by FSR itself needs "
                 "the game's targets at the output size (not yet). A sharper picture: a bigger "
                 "size in the launcher.");
        }

        Section("Sharpening");
        Toggle("Sharpen (RCAS)", s.sharpen);
        ImGui::BeginDisabled(!s.sharpen);
        Slider("##sharpness", s.sharpness, 0.0f, 2.0f);
        ImGui::EndDisabled();

        Section("Frame generation");
        static const char* frame_gens[] = {"Off", "FSR 3"};
        int frame_gen = s.frame_gen == BbSettings::FrameGenFsr3 ? 1 : 0;
        if (Segmented("framegen", frame_gen, frame_gens, 2)) {
            Store(s.frame_gen, frame_gen ? BbSettings::FrameGenFsr3 : BbSettings::FrameGenOff);
        }
        if (const char* problem = s.frame_gen_problem.load()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", problem);
            ImGui::PopStyleColor();
        } else if (frame_gen) {
            ImGui::TextDisabled("One generated frame between two of the game's");
        }

        Section("Frame rate limit");
        static const char* limits[] = {"30", "60", "120", "Off"};
        static const int limit_values[] = {30, 60, 120, 0};
        const int effective = int(EmulatorSettings.GetFrameLimit());
        int limit = -1;
        for (int i = 0; i < 4; ++i) {
            if (limit_values[i] == effective) {
                limit = i;
            }
        }
        if (Segmented("fps_limit", limit, limits, 4)) {
            Store(s.fps_limit, limit_values[limit]);
        }
        if (effective == 0) {
            ImGui::TextDisabled("No limit; above 120 FPS the game's movement timing breaks "
                                "(screen: %u Hz)", BbDisplayRefreshHz());
        } else {
            ImGui::TextDisabled("%d FPS at most (screen: %u Hz)", effective, BbDisplayRefreshHz());
        }

        Section("On-screen gamepad");
        static const char* pads[] = {"Off", "Auto", "On"};
        static const int pad_values[] = {0, 2, 1};
        int pad = s.touch_pad == 0 ? 0 : s.touch_pad == 1 ? 2 : 1;
        if (Segmented("touch_pad", pad, pads, 3)) {
            Store(s.touch_pad, pad_values[pad]);
        }
        Note("Auto: shown while no gamepad is connected. Left half: a stick under the finger; "
             "right half: drag to look around; the buttons at the edges.");
        ImGui::BeginDisabled(s.touch_pad == 0);
        ImGui::TextDisabled("Opacity");
        Slider("##touch_alpha", s.touch_pad_alpha, 0.1f, 1.0f);
        ImGui::TextDisabled("Size");
        Slider("##touch_scale", s.touch_pad_scale, 0.6f, 1.6f);
        ImGui::TextDisabled("Camera drag sensitivity");
        Slider("##touch_look", s.touch_pad_look, 0.25f, 4.0f);
        ImGui::EndDisabled();

        // g2-arm64 (issue #16): the app's controller setup window (ControllerSetup.java) over the
        // game; SDL pauses while it is open (presenting and the sound stop, the game runs on).
        Section("Controllers");
        if (ImGui::Button("Controller setup", ImVec2(-FLT_MIN, 0.0f))) {
            BbPadMap::RequestSetup();
            keep_open = false;
        }
        Note("A controller's buttons set to the PlayStation ones (also in the launcher). The game "
             "runs on meanwhile: pause it first.");

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Advanced")) {
            ImGui::TextDisabled("Output size");
            static const char* outputs[] = {"720p", "1080p", "1440p", "4K"};
            int output = s.output_res;
            if (Segmented("output", output, outputs, BbSettings::OutputCount)) {
                Store(s.output_res, output);
            }
            Toggle("Sub-pixel jitter", s.jitter);
            Toggle("Character motion vectors", s.object_motion);
            Toggle("Reactive mask", s.reactive);
            ImGui::BeginDisabled(!s.reactive);
            ImGui::TextDisabled("Scale");
            Slider("##reactive_scale", s.reactive_scale, 0.0f, 4.0f);
            ImGui::TextDisabled("Threshold");
            Slider("##reactive_threshold", s.reactive_threshold, 0.0f, 1.0f);
            ImGui::TextDisabled("Maximum");
            Slider("##reactive_max", s.reactive_max, 0.0f, 1.0f);
            ImGui::EndDisabled();
        }

        const bool restart = s.object_motion != s.startup_object_motion ||
                             BbSettings::ResolutionNeedsRestart();
        if (restart) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            ImGui::TextWrapped("Some of these take effect after a restart.");
            ImGui::PopStyleColor();
            if (ImGui::Button("Apply and restart the game", ImVec2(-FLT_MIN, 0.0f))) {
                BbSettings::Save();
                runtime_restart();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Close##bottom", ImVec2(-FLT_MIN, 0.0f))) {
            keep_open = false;
        }
        // g2-arm64: Quit game (issue #29: the PS4 games have no exit of their own, and swiping the
        // app away in Recents was the only way out). The PC menu's exit: a save in progress is
        // finished first (at most 5 s), then the process ends and the game's task leaves Recents.
        if (QuitRequested()) {
            ImGui::BeginDisabled();
            ImGui::Button("Quitting...", ImVec2(-FLT_MIN, 0.0f));
            ImGui::EndDisabled();
        } else if (!confirm_quit) {
            if (ImGui::Button("Quit game", ImVec2(-FLT_MIN, 0.0f))) {
                confirm_quit = focus_cancel = true;
            }
        } else {
            Note("Quit the game? Progress since the last save is lost; a save in progress is "
                 "finished first.");
            const float half =
                (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Quit", ImVec2(half, 0.0f))) {
                if (dirty) {
                    BbSettings::Save();
                    dirty = false;
                }
                RequestQuit();
                confirm_quit = false;
            }
            ImGui::SameLine();
            if (focus_cancel) {
                ImGui::SetKeyboardFocusHere();
                focus_cancel = false;
            }
            if (ImGui::Button("Cancel##quit", ImVec2(half, 0.0f))) {
                confirm_quit = false;
            }
        }
        ImGui::TextDisabled("Swipe in from the right edge or L3+R3 to open");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(8);
    if (dirty && (!keep_open || ImGui::GetTime() - last_change > 1.0)) {
        BbSettings::Save();
        dirty = false;
    }
}

TouchAction AndroidTouch(const SDL_Event& event, bool open) {
    static SDL_FingerID edge_finger = 0;
    static bool edge_tracking = false;
    static float edge_start_x = 0.0f;
    static float down_x = 0.0f, down_y = 0.0f;
    static float last_y = 0.0f;
    static bool down_outside = false, down_inside = false;
    constexpr float EdgeZone = 0.03f;   // of the screen width
    constexpr float SwipeLength = 0.08f;
    constexpr float TapSlop = 0.02f;
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
        if (!open && event.tfinger.x >= 1.0f - EdgeZone) {
            edge_finger = event.tfinger.fingerID;
            edge_tracking = true;
            edge_start_x = event.tfinger.x;
        }
        down_x = event.tfinger.x;
        down_y = last_y = event.tfinger.y;
        down_outside = open && event.tfinger.x < panel_left_norm.load();
        down_inside = open && !down_outside;
        return TouchAction::None;
    case SDL_EVENT_FINGER_MOTION:
        if (edge_tracking && event.tfinger.fingerID == edge_finger &&
            event.tfinger.x <= edge_start_x - SwipeLength) {
            edge_tracking = false;
            return TouchAction::Open;
        }
        if (down_inside) {
            // Scroll by the finger's travel once it is clearly a drag, not a tap.
            if (std::abs(event.tfinger.y - down_y) >= TapSlop) {
                pending_scroll.fetch_add(event.tfinger.y - last_y);
            }
            last_y = event.tfinger.y;
        }
        return TouchAction::None;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (edge_tracking && event.tfinger.fingerID == edge_finger) {
            edge_tracking = false;
        }
        down_inside = false;
        if (open && down_outside && event.type == SDL_EVENT_FINGER_UP &&
            std::abs(event.tfinger.x - down_x) < TapSlop &&
            std::abs(event.tfinger.y - down_y) < TapSlop) {
            down_outside = false;
            return TouchAction::Close;
        }
        return TouchAction::None;
    default:
        return TouchAction::None;
    }
}

} // namespace BbOverlay
