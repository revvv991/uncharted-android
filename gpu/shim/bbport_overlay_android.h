// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64: the in-game menu for touch screens (bbport_overlay_android.cpp).
#pragma once

#include <SDL3/SDL.h>

namespace BbOverlay {

/// The settings panel on the right edge of the screen, finger-sized. `keep_open` goes false
/// when its Close button is pressed. `base_scale` is the overlay's display scale (1080p = 1),
/// `frame_ms` the recent frame time.
void AndroidMenu(float base_scale, float frame_ms, bool& keep_open);

enum class TouchAction { None, Open, Close };

/// Finger events: a swipe in from the right edge opens the menu, a tap outside the panel closes
/// it. Mouse events SDL makes from the same touches still reach ImGui for the panel itself.
TouchAction AndroidTouch(const SDL_Event& event, bool open);

/// Quit game (bbport_overlay.cpp, the PC menu's exit): a save in progress is finished first (at
/// most 5 s), then the window thread ends the process, on Android after taking the game's task
/// out of Recents (bbgpu.cpp). QuitRequested is true from the request on.
void RequestQuit();
bool QuitRequested();

} // namespace BbOverlay
