// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal, Android };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    /// g2-arm64 (Android): the window system info with the current native window: Android takes
    /// the window away when the app leaves the foreground (the screen goes off) and SDL gets a
    /// new one when it comes back; null while there is none. Elsewhere GetWindowInfo().
    WindowSystemInfo CurrentWindowInfo() const;
    /// g2-arm64 (Android): the app is in the background (set by an SDL event watch the moment
    /// SDL queues the event, before its event pump blocks for the pause).
    bool InBackground() const { return background.load(std::memory_order_acquire); }
    /// g2-arm64 (Android): steady_clock time (ns) the app last came back to the foreground, 0
    /// before that: the stall report counts from it (no flips come in the background).
    s64 ForegroundSinceNs() const { return foreground_ns.load(std::memory_order_acquire); }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    SDL_Window* window{};
    WindowSystemInfo window_info{};
    std::atomic<bool> background{false};
    std::atomic<s64> foreground_ns{0};
    static bool OnAppEvent(void* userdata, union SDL_Event* event);
};

} // namespace Frontend
