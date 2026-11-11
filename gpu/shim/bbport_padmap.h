// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64 (issue #16): controller setups made in the app's Controller setup window
// (android/scripts/g2/game_app/ControllerSetup.java), applied to SDL. Android only.
#pragma once

#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>

namespace BbPadMap {

/// Reads the setups (files/controllers.txt, next to bbport.ini).
void Init();
/// A joystick SDL opened: its setup applied when there is one. A joystick the game still cannot
/// read (no mapping) is opened to hear it, and the notice to show comes back (empty: none).
std::string Added(SDL_JoystickID id);
void Removed(SDL_JoystickID id);
/// A joystick's event: a press on a controller nobody has set up comes back as a notice, at most
/// every 30 s (empty: none).
std::string Event(const SDL_Event& event);
/// Every pass of the window thread: opens the setup window when the panel asked for it, applies
/// the setups again once the file has changed (the window closed over the game).
void Poll();
/// The in-game panel's Controller setup item: the window opens on the window thread's next pass.
void RequestSetup();

} // namespace BbPadMap
