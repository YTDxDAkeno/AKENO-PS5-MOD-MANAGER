// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <vector>

#include <SDL2/SDL.h>

#include "akeno/ui/Input.hpp"

namespace akeno::ui::sdl {

// Maps the DualSense (through SDL's GameController API, or the PS5 SDL port's raw joystick
// button order), the left stick, and a keyboard onto UI actions.
class SdlInput {
public:
    explicit SdlInput(bool useRawPs5Mapping) : useRawPs5Mapping_(useRawPs5Mapping) {}
    ~SdlInput();
    SdlInput(const SdlInput&) = delete;
    SdlInput& operator=(const SdlInput&) = delete;

    void openConnectedDevices();
    void handleEvent(const SDL_Event& event, double now, std::vector<Action>& out);
    void update(double now, std::vector<Action>& out);

private:
    void openDevice(int index);
    void press(Action action, double now, std::vector<Action>& out);
    void release(Action action);
    void stickAxis(bool vertical, int value, double now, std::vector<Action>& out);
    void hat(Uint8 value, double now, std::vector<Action>& out);

    bool useRawPs5Mapping_;
    KeyRepeater repeater_;
    std::map<SDL_JoystickID, SDL_GameController*> controllers_;
    std::map<SDL_JoystickID, SDL_Joystick*> rawJoysticks_;
    int stickX_ = 0;  // -1, 0, 1
    int stickY_ = 0;
    Uint8 hat_ = SDL_HAT_CENTERED;
    bool leftTrigger_ = false;
    bool rightTrigger_ = false;
};

}  // namespace akeno::ui::sdl
