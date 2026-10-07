// SPDX-License-Identifier: GPL-3.0-or-later
// Abstract controller actions. The SDL layer maps DualSense buttons, the left stick and the
// keyboard onto these; screens never see raw input.
#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace akeno::ui {

enum class Action {
    Up,
    Down,
    Left,
    Right,
    Confirm,    // Cross
    Back,       // Circle
    Secondary,  // Triangle
    Tertiary,   // Square
    Options,    // OPTIONS
    PrevTab,    // L1
    NextTab,    // R1
    PageUp,     // L2
    PageDown,   // R2
};

inline constexpr std::size_t kActionCount = 13;

std::string_view toString(Action action) noexcept;
bool isRepeatable(Action action) noexcept;

// Button index -> action for the raw joystick exposed by the ps5-payload-dev SDL port
// (src/joystick/ps5/SDL_ps5joystick.c at ee4c47d: 0 Cross, 1 Circle, 2 Square, 3 Triangle,
// 6 Options, 9 L1, 10 R1, 11-14 D-pad up/down/left/right, 15 L2, 16 R2).
std::optional<Action> ps5RawButtonAction(int button) noexcept;

// Turns "button held" into repeated actions, like a console menu: one action on press, then
// a pause, then a steady repeat while held.
class KeyRepeater {
public:
    explicit KeyRepeater(double initialDelaySeconds = 0.35, double intervalSeconds = 0.08)
        : initialDelay_(initialDelaySeconds), interval_(intervalSeconds) {}

    // Returns the action to deliver for a press (always the action itself).
    Action press(Action action, double now);
    void release(Action action);
    void releaseAll();
    // Actions due because a repeatable button is still held.
    std::vector<Action> update(double now);

private:
    struct Held {
        bool down = false;
        double nextFire = 0.0;
    };
    double initialDelay_;
    double interval_;
    std::array<Held, kActionCount> held_{};
};

}  // namespace akeno::ui
