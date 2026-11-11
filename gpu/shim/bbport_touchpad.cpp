// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64: the on-screen gamepad (bbport_touchpad.h). Layout in units of the display height
// (1080 at 1080p), anchored to the screen's edges so phones of any aspect ratio get the same
// controls: the face buttons bottom right, L1/L2 and R1/R2 in the top corners, the D-pad on the
// left, L3/R3 at the bottom centre, the touch pad button and Options at the top. A finger that
// lands on no control drives the left stick on the left half (the stick appears under it) or
// turns the camera on the right half (its movement becomes right stick deflection for that
// frame, as mobile shooters do). Fingers are followed by id, so several work at once.
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "imgui.h"

#include "bbport_settings.h"
#include "bbport_touchpad.h"

extern "C" void runtime_touch_pad(uint32_t buttons, const int sticks[4], uint8_t l2, uint8_t r2);
extern "C" int runtime_gamepad_connected(void);

namespace BbOverlay {

namespace {

// OrbisPadData button bits (src/runtime_pad.c).
enum : uint32_t {
    BtnL3 = 0x2, BtnR3 = 0x4, BtnOptions = 0x8, BtnUp = 0x10, BtnRight = 0x20, BtnDown = 0x40,
    BtnLeft = 0x80, BtnL2 = 0x100, BtnR2 = 0x200, BtnL1 = 0x400, BtnR1 = 0x800,
    BtnTriangle = 0x1000, BtnCircle = 0x2000, BtnCross = 0x4000, BtnSquare = 0x8000,
    BtnTouchpad = 0x100000,
};

enum class Shape { Circle, Pill };
/// What is drawn inside: a text label, or a PlayStation glyph made of lines (fonts lack them).
enum class Glyph { Text, Cross, Circle, Square, Triangle };

/// A fixed control: its centre from an edge (ax 0 = the left edge, 1 = the right, 0.5 the
/// middle; ay the same for the top and bottom), its size, all in units of the display height.
struct Control {
    const char* label;
    uint32_t bit;
    float ax, ox, ay, oy; // centre = (ax * W + ox * H, ay * H + oy * H)
    float w, h;           // a pill's extent; a circle's diameter in w
    Shape shape;
    Glyph glyph;
};

constexpr Control Controls[] = {
    // the face buttons: a diamond at the bottom right
    {"", BtnTriangle, 1.0f, -0.24f, 1.0f, -0.405f, 0.11f, 0.11f, Shape::Circle, Glyph::Triangle},
    {"", BtnCircle, 1.0f, -0.135f, 1.0f, -0.30f, 0.11f, 0.11f, Shape::Circle, Glyph::Circle},
    {"", BtnCross, 1.0f, -0.24f, 1.0f, -0.195f, 0.11f, 0.11f, Shape::Circle, Glyph::Cross},
    {"", BtnSquare, 1.0f, -0.345f, 1.0f, -0.30f, 0.11f, 0.11f, Shape::Circle, Glyph::Square},
    // shoulders and triggers in the top corners
    {"L2", BtnL2, 0.0f, 0.14f, 0.0f, 0.085f, 0.22f, 0.085f, Shape::Pill, Glyph::Text},
    {"L1", BtnL1, 0.0f, 0.14f, 0.0f, 0.20f, 0.22f, 0.085f, Shape::Pill, Glyph::Text},
    {"R2", BtnR2, 1.0f, -0.14f, 0.0f, 0.085f, 0.22f, 0.085f, Shape::Pill, Glyph::Text},
    {"R1", BtnR1, 1.0f, -0.14f, 0.0f, 0.20f, 0.22f, 0.085f, Shape::Pill, Glyph::Text},
    // the top middle: the touch pad button and Options
    {"PAD", BtnTouchpad, 0.5f, -0.16f, 0.0f, 0.06f, 0.17f, 0.07f, Shape::Pill, Glyph::Text},
    {"OPTIONS", BtnOptions, 0.5f, 0.16f, 0.0f, 0.06f, 0.21f, 0.07f, Shape::Pill, Glyph::Text},
    // the stick clicks at the bottom middle
    {"L3", BtnL3, 0.5f, -0.14f, 1.0f, -0.075f, 0.09f, 0.09f, Shape::Circle, Glyph::Text},
    {"R3", BtnR3, 0.5f, 0.14f, 1.0f, -0.075f, 0.09f, 0.09f, Shape::Circle, Glyph::Text},
};
constexpr int ControlCount = int(sizeof(Controls) / sizeof(Controls[0]));

// The D-pad: a plus on the left, above the stick's usual place.
constexpr float DpadX = 0.22f, DpadY = 0.47f; // centre, in H (x from the left edge)
constexpr float DpadArm = 0.105f, DpadWidth = 0.075f, DpadDead = 0.025f;

// The floating left stick and the camera drag.
constexpr float StickRadius = 0.11f, KnobRadius = 0.05f; // in H
constexpr float StickHintX = 0.28f, StickHintY = 0.70f;  // the idle hint, in H
// The camera drag: full right stick deflection at this finger speed (display heights per second)
// times the sensitivity setting; slower drags turn proportionally slower. Independent of the
// frame rate (the deflection is the finger's speed over the time since the last frame).
constexpr float LookFullSpeed = 0.5f;
constexpr float EdgeZone = 0.03f;   // the menu's swipe-in strip (AndroidTouch)
constexpr double MinHold = 0.09;    // seconds a tapped button stays pressed (a tap can be shorter than a frame)

enum class Kind { None, Button, Dpad, Stick, Look };

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Finger {
    SDL_FingerID id = 0;
    bool down = false;
    bool lifted = false;            // released before MinHold: still pressed until then
    double down_time = 0.0;
    Kind kind = Kind::None;
    int control = -1;               // Controls index for Kind::Button
    float x0 = 0, y0 = 0;           // where it landed (normalized)
    float x = 0, y = 0;             // where it is
    float look_dx = 0, look_dy = 0; // normalized travel since the last frame (Kind::Look)
    float look_vx = 0, look_vy = 0; // the smoothed deflection (Kind::Look)
};

std::mutex fingers_mutex;
Finger fingers[10];
bool was_active = false;
double last_push = 0.0;

float Width = 1920.0f, Height = 1080.0f;

ImVec2 Centre(const Control& c) {
    return ImVec2(c.ax * Width + c.ox * Height, c.ay * Height + c.oy * Height);
}

bool Hits(const Control& c, float px, float py, float scale) {
    const ImVec2 centre = Centre(c);
    const float hw = c.w * Height * scale * 0.5f, hh = c.h * Height * scale * 0.5f;
    if (c.shape == Shape::Circle) {
        const float dx = px - centre.x, dy = py - centre.y;
        return dx * dx + dy * dy <= hw * hw * 1.25f; // a little margin for fingers
    }
    return std::abs(px - centre.x) <= hw * 1.15f && std::abs(py - centre.y) <= hh * 1.3f;
}

/// D-pad bits for a point, 0 outside the plus.
uint32_t DpadBits(float px, float py, float scale) {
    const float cx = DpadX * Height, cy = DpadY * Height;
    const float dx = (px - cx) / (Height * scale), dy = (py - cy) / (Height * scale);
    const float ax = std::abs(dx), ay = std::abs(dy);
    if (ax > DpadArm + 0.02f || ay > DpadArm + 0.02f) return 0;
    if (ax > DpadWidth * 0.5f + 0.02f && ay > DpadWidth * 0.5f + 0.02f) {
        // a corner: both arms when the finger is clearly diagonal, nothing far out
        if (ax < DpadArm * 0.9f && ay < DpadArm * 0.9f) {
            return (dx < 0 ? BtnLeft : BtnRight) | (dy < 0 ? BtnUp : BtnDown);
        }
        return 0;
    }
    if (ax < DpadDead && ay < DpadDead) return 0;
    if (ax >= ay) return dx < 0 ? BtnLeft : BtnRight;
    return dy < 0 ? BtnUp : BtnDown;
}

Finger* Find(SDL_FingerID id) {
    for (auto& f : fingers) {
        if (f.down && !f.lifted && f.id == id) return &f;
    }
    return nullptr;
}

Finger* Free() {
    for (auto& f : fingers) {
        if (!f.down) return &f;
    }
    return nullptr;
}

// g2-arm64 (issue #34): the full push comes at this share of the ring's radius. With the full
// push at the ring itself, a thumb that stopped short of it kept Nate walking in Uncharted 2 and
// 3: U2 runs from about 85% of a full push (80% walks, 90% runs: the G2, 2026-11-10), U1 earlier.
// At 0.75 a run starts at about two thirds of the ring. BB_TOUCH_STICK_FULL=<0.5..1> (1: at the
// ring, as in 0.3.1).
float StickFullShare() {
    static const float share = [] {
        const char* env = std::getenv("BB_TOUCH_STICK_FULL");
        const float v = env && *env ? float(std::atof(env)) : 0.75f;
        return std::clamp(v, 0.5f, 1.0f);
    }();
    return share;
}

/// A stick finger's deflection: its travel from where it landed over the full push's distance,
/// inside the unit circle.
void StickDeflection(const Finger& f, float scale, float& dx, float& dy) {
    const float full = StickRadius * Height * scale * StickFullShare();
    dx = (f.x - f.x0) * Width / full;
    dy = (f.y - f.y0) * Height / full;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1.0f) {
        dx /= len;
        dy /= len;
    }
}

/// A deflection (-1..1) as the pad's byte: 128 + d * 128 within 0..255, as runtime_pad.c's axis()
/// makes a physical stick's (a full push up or left is 0, as on a DualShock; 128 + d * 127
/// stopped at 1).
int StickByte(float d) {
    return std::clamp(int(std::lround(128.0f + d * 128.0f)), 0, 255);
}

/// The pad state of this frame to the runtime (fingers_mutex held).
void Push(bool neutral) {
    uint32_t buttons = 0;
    int sticks[4] = {-1, -1, -1, -1};
    uint8_t l2 = 0, r2 = 0;
    const double now = Now();
    const float dt = float(std::clamp(now - last_push, 0.004, 0.1));
    last_push = now;
    if (!neutral) {
        const float scale = BbSettings::Get().touch_pad_scale.load();
        const float sensitivity = std::clamp(BbSettings::Get().touch_pad_look.load(), 0.25f, 4.0f);
        for (auto& f : fingers) {
            if (f.down && f.lifted && now - f.down_time >= MinHold) {
                f.down = f.lifted = false; // a short tap held long enough
            }
            if (!f.down) continue;
            switch (f.kind) {
            case Kind::Button:
                buttons |= Controls[f.control].bit;
                if (Controls[f.control].bit == BtnL2) l2 = 255;
                if (Controls[f.control].bit == BtnR2) r2 = 255;
                break;
            case Kind::Dpad:
                buttons |= DpadBits(f.x * Width, f.y * Height, scale);
                break;
            case Kind::Stick: {
                float dx, dy;
                StickDeflection(f, scale, dx, dy);
                sticks[0] = StickByte(dx);
                sticks[1] = StickByte(dy);
                break;
            }
            case Kind::Look: {
                // the finger's speed since the last frame: the camera turns while it moves
                const float full = LookFullSpeed * Height / sensitivity; // pixels per second
                // finger events and frames tick at different rates: smooth over ~2 frames
                f.look_vx = 0.5f * f.look_vx + 0.5f * (f.look_dx * Width / dt / full);
                f.look_vy = 0.5f * f.look_vy + 0.5f * (f.look_dy * Height / dt / full);
                const float dx = std::clamp(f.look_vx, -1.0f, 1.0f);
                const float dy = std::clamp(f.look_vy, -1.0f, 1.0f);
                f.look_dx = f.look_dy = 0.0f;
                sticks[2] = StickByte(dx);
                sticks[3] = StickByte(dy);
                break;
            }
            case Kind::None:
                break;
            }
        }
        for (int& s : sticks) {
            if (s >= 0) s = std::clamp(s, 0, 255);
        }
    }
    runtime_touch_pad(buttons, sticks, l2, r2);
}

void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 c, float r, ImU32 colour, float thick) {
    switch (glyph) {
    case Glyph::Cross:
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), colour, thick);
        dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), colour, thick);
        break;
    case Glyph::Circle:
        dl->AddCircle(c, r, colour, 32, thick);
        break;
    case Glyph::Square:
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), colour, 0.0f, 0, thick);
        break;
    case Glyph::Triangle:
        dl->AddTriangle(ImVec2(c.x, c.y - r * 1.1f), ImVec2(c.x + r * 1.05f, c.y + r * 0.8f),
                        ImVec2(c.x - r * 1.05f, c.y + r * 0.8f), colour, thick);
        break;
    case Glyph::Text:
        break;
    }
}

void Draw(float alpha, float scale) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float H = Height;
    const auto col = [alpha](int a) { return IM_COL32(255, 255, 255, int(alpha * float(a))); };
    const float text_size = 0.04f * H * scale;
    const auto shadow = [alpha](int a) { return IM_COL32(0, 0, 0, int(alpha * float(a))); };
    const auto label = [&](const char* text, ImVec2 at, float size) {
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const ImVec2 pos(at.x - ts.x * 0.5f, at.y - ts.y * 0.5f);
        const float o = std::max(1.0f, size * 0.05f); // a dark shadow: white text over a bright scene
        dl->AddText(font, size, ImVec2(pos.x + o, pos.y + o), shadow(200), text);
        dl->AddText(font, size, pos, col(235), text);
    };
    uint32_t held = 0;
    bool stick_down = false;
    ImVec2 stick_base, stick_knob;
    {
        std::scoped_lock lock{fingers_mutex};
        for (const auto& f : fingers) {
            if (!f.down) continue;
            if (f.kind == Kind::Button) held |= Controls[f.control].bit;
            if (f.kind == Kind::Dpad) held |= DpadBits(f.x * Width, f.y * H, scale);
            if (f.kind == Kind::Stick) {
                stick_down = true;
                stick_base = ImVec2(f.x0 * Width, f.y0 * H);
                // the knob shows the deflection: on the ring at a full push
                float dx, dy;
                StickDeflection(f, scale, dx, dy);
                const float r = StickRadius * H * scale;
                stick_knob = ImVec2(stick_base.x + dx * r, stick_base.y + dy * r);
            }
        }
    }
    const float line = 2.0f * scale;
    for (const auto& c : Controls) {
        const ImVec2 centre = Centre(c);
        const bool on = held & c.bit;
        if (c.shape == Shape::Circle) {
            const float r = c.w * H * scale * 0.5f;
            dl->AddCircleFilled(centre, r, col(on ? 150 : 55), 48);
            dl->AddCircle(centre, r, col(on ? 240 : 150), 48, line);
            if (c.glyph != Glyph::Text) {
                DrawGlyph(dl, c.glyph, ImVec2(centre.x + line, centre.y + line), r * 0.42f,
                          shadow(200), line * 1.5f);
                DrawGlyph(dl, c.glyph, centre, r * 0.42f, col(230), line * 1.5f);
            }
        } else {
            const ImVec2 a(centre.x - c.w * H * scale * 0.5f, centre.y - c.h * H * scale * 0.5f);
            const ImVec2 b(centre.x + c.w * H * scale * 0.5f, centre.y + c.h * H * scale * 0.5f);
            const float rounding = c.h * H * scale * 0.5f;
            dl->AddRectFilled(a, b, col(on ? 150 : 55), rounding);
            dl->AddRect(a, b, col(on ? 240 : 150), rounding, 0, line);
        }
        if (c.glyph == Glyph::Text) {
            label(c.label, centre, c.shape == Shape::Circle ? text_size : text_size * 0.8f);
        }
    }
    // the D-pad
    {
        const ImVec2 c(DpadX * H, DpadY * H);
        const float arm = DpadArm * H * scale, w = DpadWidth * H * scale * 0.5f;
        const struct { uint32_t bit; ImVec2 a, b; } arms[] = {
            {BtnUp, ImVec2(c.x - w, c.y - arm), ImVec2(c.x + w, c.y - w * 0.4f)},
            {BtnDown, ImVec2(c.x - w, c.y + w * 0.4f), ImVec2(c.x + w, c.y + arm)},
            {BtnLeft, ImVec2(c.x - arm, c.y - w), ImVec2(c.x - w * 0.4f, c.y + w)},
            {BtnRight, ImVec2(c.x + w * 0.4f, c.y - w), ImVec2(c.x + arm, c.y + w)},
        };
        for (const auto& a : arms) {
            const bool on = held & a.bit;
            dl->AddRectFilled(a.a, a.b, col(on ? 150 : 55), w * 0.35f);
            dl->AddRect(a.a, a.b, col(on ? 240 : 150), w * 0.35f, 0, line);
        }
    }
    // the left stick: under the finger, or a hint of where it usually is
    {
        const float r = StickRadius * H * scale, k = KnobRadius * H * scale;
        if (stick_down) {
            dl->AddCircleFilled(stick_base, r, col(45), 64);
            dl->AddCircle(stick_base, r, col(170), 64, line);
            dl->AddCircleFilled(stick_knob, k, col(170), 48);
        } else {
            const ImVec2 hint(StickHintX * H, StickHintY * H);
            dl->AddCircle(hint, r, col(70), 64, line);
            dl->AddCircleFilled(hint, k, col(60), 48);
        }
    }
}

} // namespace

bool TouchPadActive() {
    const int mode = BbSettings::Get().touch_pad.load();
    return mode == 1 || (mode == 2 && !runtime_gamepad_connected());
}

bool TouchPadEvent(const SDL_Event& event) {
    std::scoped_lock lock{fingers_mutex};
    const float scale = BbSettings::Get().touch_pad_scale.load();
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        const float px = event.tfinger.x * Width, py = event.tfinger.y * Height;
        int control = -1;
        for (int i = 0; i < ControlCount; ++i) {
            if (Hits(Controls[i], px, py, scale)) {
                control = i;
                break;
            }
        }
        // the menu's swipe-in strip, unless the finger is on a control there (R1, R2)
        if (control < 0 && event.tfinger.x >= 1.0f - EdgeZone) return false;
        Finger* f = Free();
        if (!f) return false;
        *f = Finger{};
        f->id = event.tfinger.fingerID;
        f->down = true;
        f->down_time = Now();
        f->x = f->x0 = event.tfinger.x;
        f->y = f->y0 = event.tfinger.y;
        if (control >= 0) {
            f->kind = Kind::Button;
            f->control = control;
        } else if (DpadBits(px, py, scale)) {
            f->kind = Kind::Dpad;
        } else {
            f->kind = event.tfinger.x < 0.5f ? Kind::Stick : Kind::Look;
        }
        return true;
    }
    case SDL_EVENT_FINGER_MOTION: {
        Finger* f = Find(event.tfinger.fingerID);
        if (!f) return false;
        if (f->kind == Kind::Look) {
            f->look_dx += event.tfinger.x - f->x;
            f->look_dy += event.tfinger.y - f->y;
        }
        f->x = event.tfinger.x;
        f->y = event.tfinger.y;
        return true;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        Finger* f = Find(event.tfinger.fingerID);
        if (!f) return false;
        if ((f->kind == Kind::Button || f->kind == Kind::Dpad) && Now() - f->down_time < MinHold &&
            event.type == SDL_EVENT_FINGER_UP) {
            f->lifted = true; // pressed until MinHold (Push)
        } else {
            f->down = false;
        }
        return true;
    }
    default:
        return false;
    }
}

void TouchPadFrame(float width, float height) {
    Width = width;
    Height = height;
    const bool active = TouchPadActive();
    if (!active) {
        if (was_active) TouchPadReset();
        was_active = false;
        return;
    }
    was_active = true;
    const auto& s = BbSettings::Get();
    const float alpha = std::clamp(s.touch_pad_alpha.load(), 0.1f, 1.0f);
    const float scale = std::clamp(s.touch_pad_scale.load(), 0.6f, 1.6f);
    {
        std::scoped_lock lock{fingers_mutex};
        Push(false);
    }
    Draw(alpha, scale);
}

void TouchPadReset() {
    std::scoped_lock lock{fingers_mutex};
    for (auto& f : fingers) f.down = f.lifted = false;
    Push(true);
}

} // namespace BbOverlay
