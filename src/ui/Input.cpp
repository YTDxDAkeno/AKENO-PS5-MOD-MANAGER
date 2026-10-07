// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/Input.hpp"

namespace akeno::ui {

std::string_view toString(Action action) noexcept {
    switch (action) {
        case Action::Up: return "up";
        case Action::Down: return "down";
        case Action::Left: return "left";
        case Action::Right: return "right";
        case Action::Confirm: return "confirm";
        case Action::Back: return "back";
        case Action::Secondary: return "secondary";
        case Action::Tertiary: return "tertiary";
        case Action::Options: return "options";
        case Action::PrevTab: return "prev-tab";
        case Action::NextTab: return "next-tab";
        case Action::PageUp: return "page-up";
        case Action::PageDown: return "page-down";
    }
    return "?";
}

bool isRepeatable(Action action) noexcept {
    switch (action) {
        case Action::Up:
        case Action::Down:
        case Action::Left:
        case Action::Right:
        case Action::PageUp:
        case Action::PageDown:
            return true;
        default:
            return false;
    }
}

std::optional<Action> ps5RawButtonAction(int button) noexcept {
    switch (button) {
        case 0: return Action::Confirm;
        case 1: return Action::Back;
        case 2: return Action::Tertiary;
        case 3: return Action::Secondary;
        case 6: return Action::Options;
        case 9: return Action::PrevTab;
        case 10: return Action::NextTab;
        case 11: return Action::Up;
        case 12: return Action::Down;
        case 13: return Action::Left;
        case 14: return Action::Right;
        case 15: return Action::PageUp;
        case 16: return Action::PageDown;
        default: return std::nullopt;
    }
}

Action KeyRepeater::press(Action action, double now) {
    auto& held = held_[static_cast<std::size_t>(action)];
    if (isRepeatable(action)) {
        held.down = true;
        held.nextFire = now + initialDelay_;
    }
    return action;
}

void KeyRepeater::release(Action action) { held_[static_cast<std::size_t>(action)].down = false; }

void KeyRepeater::releaseAll() {
    for (auto& held : held_) {
        held.down = false;
    }
}

std::vector<Action> KeyRepeater::update(double now) {
    std::vector<Action> due;
    for (std::size_t i = 0; i < held_.size(); ++i) {
        auto& held = held_[i];
        if (!held.down) {
            continue;
        }
        // Deliver at most one repeat per update so a stalled frame cannot flood the UI.
        if (now >= held.nextFire) {
            due.push_back(static_cast<Action>(i));
            held.nextFire = now + interval_;
        }
    }
    return due;
}

}  // namespace akeno::ui
