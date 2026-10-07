// SPDX-License-Identifier: GPL-3.0-or-later
#include "SdlInput.hpp"

#include "akeno/logging/Logger.hpp"

namespace akeno::ui::sdl {

using logging::logger;

namespace {

constexpr int kStickPress = 20000;
constexpr int kStickRelease = 12000;
constexpr int kTriggerPress = 20000;

std::optional<Action> controllerButton(Uint8 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return Action::Confirm;
        case SDL_CONTROLLER_BUTTON_B: return Action::Back;
        case SDL_CONTROLLER_BUTTON_X: return Action::Tertiary;
        case SDL_CONTROLLER_BUTTON_Y: return Action::Secondary;
        case SDL_CONTROLLER_BUTTON_START: return Action::Options;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Action::PrevTab;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Action::NextTab;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Action::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Action::Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Action::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Action::Right;
        default: return std::nullopt;
    }
}

std::optional<Action> keyboardKey(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP: return Action::Up;
        case SDLK_DOWN: return Action::Down;
        case SDLK_LEFT: return Action::Left;
        case SDLK_RIGHT: return Action::Right;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: return Action::Confirm;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE: return Action::Back;
        case SDLK_t: return Action::Secondary;
        case SDLK_y: return Action::Tertiary;
        case SDLK_TAB:
        case SDLK_F5: return Action::Options;
        case SDLK_q: return Action::PrevTab;
        case SDLK_e: return Action::NextTab;
        case SDLK_PAGEUP: return Action::PageUp;
        case SDLK_PAGEDOWN: return Action::PageDown;
        default: return std::nullopt;
    }
}

}  // namespace

SdlInput::~SdlInput() {
    for (auto& [id, controller] : controllers_) SDL_GameControllerClose(controller);
    for (auto& [id, joystick] : rawJoysticks_) SDL_JoystickClose(joystick);
}

void SdlInput::openConnectedDevices() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) openDevice(i);
}

void SdlInput::openDevice(int index) {
    if (SDL_IsGameController(index)) {
        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (controller != nullptr) {
            SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
            if (controllers_.count(id) != 0) {
                SDL_GameControllerClose(controller);
                return;
            }
            controllers_[id] = controller;
            logger().info("input", std::string("controller connected: ") + SDL_GameControllerName(controller));
        }
        return;
    }
    if (!useRawPs5Mapping_) {
        return;  // unknown joysticks have no reliable button layout on desktop systems
    }
    SDL_Joystick* joystick = SDL_JoystickOpen(index);
    if (joystick != nullptr) {
        SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
        if (rawJoysticks_.count(id) != 0) {
            SDL_JoystickClose(joystick);
            return;
        }
        rawJoysticks_[id] = joystick;
        const char* name = SDL_JoystickName(joystick);
        logger().info("input", std::string("pad connected (raw PS5 mapping): ") + (name != nullptr ? name : "?"));
    }
}

void SdlInput::press(Action action, double now, std::vector<Action>& out) { out.push_back(repeater_.press(action, now)); }

void SdlInput::release(Action action) { repeater_.release(action); }

void SdlInput::stickAxis(bool vertical, int value, double now, std::vector<Action>& out) {
    int& state = vertical ? stickY_ : stickX_;
    int next = state;
    if (value <= -kStickPress) next = -1;
    else if (value >= kStickPress) next = 1;
    else if (value > -kStickRelease && value < kStickRelease) next = 0;
    if (next == state) return;
    const Action negative = vertical ? Action::Up : Action::Left;
    const Action positive = vertical ? Action::Down : Action::Right;
    if (state == -1) release(negative);
    if (state == 1) release(positive);
    state = next;
    if (state == -1) press(negative, now, out);
    if (state == 1) press(positive, now, out);
}

void SdlInput::hat(Uint8 value, double now, std::vector<Action>& out) {
    auto edge = [&](Uint8 mask, Action action) {
        const bool was = (hat_ & mask) != 0;
        const bool is = (value & mask) != 0;
        if (is && !was) press(action, now, out);
        if (!is && was) release(action);
    };
    edge(SDL_HAT_UP, Action::Up);
    edge(SDL_HAT_DOWN, Action::Down);
    edge(SDL_HAT_LEFT, Action::Left);
    edge(SDL_HAT_RIGHT, Action::Right);
    hat_ = value;
}

void SdlInput::handleEvent(const SDL_Event& event, double now, std::vector<Action>& out) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_JOYDEVICEADDED:
            openDevice(event.type == SDL_CONTROLLERDEVICEADDED ? event.cdevice.which : event.jdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED: {
            auto it = controllers_.find(event.cdevice.which);
            if (it != controllers_.end()) {
                SDL_GameControllerClose(it->second);
                controllers_.erase(it);
                repeater_.releaseAll();
            }
            break;
        }
        case SDL_JOYDEVICEREMOVED: {
            auto it = rawJoysticks_.find(event.jdevice.which);
            if (it != rawJoysticks_.end()) {
                SDL_JoystickClose(it->second);
                rawJoysticks_.erase(it);
                repeater_.releaseAll();
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
            if (auto action = controllerButton(event.cbutton.button)) press(*action, now, out);
            break;
        case SDL_CONTROLLERBUTTONUP:
            if (auto action = controllerButton(event.cbutton.button)) release(*action);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) stickAxis(false, event.caxis.value, now, out);
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) stickAxis(true, event.caxis.value, now, out);
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                bool& held = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? leftTrigger_ : rightTrigger_;
                const Action action = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? Action::PageUp : Action::PageDown;
                const bool pressed = event.caxis.value > kTriggerPress;
                if (pressed && !held) press(action, now, out);
                if (!pressed && held) release(action);
                held = pressed;
            }
            break;
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            if (rawJoysticks_.count(event.jbutton.which) != 0) {
                if (auto action = ps5RawButtonAction(event.jbutton.button)) {
                    if (event.type == SDL_JOYBUTTONDOWN) press(*action, now, out);
                    else release(*action);
                }
            }
            break;
        case SDL_JOYAXISMOTION:
            if (rawJoysticks_.count(event.jaxis.which) != 0 && event.jaxis.axis <= 1) {
                stickAxis(event.jaxis.axis == 1, event.jaxis.value, now, out);
            }
            break;
        case SDL_JOYHATMOTION:
            if (rawJoysticks_.count(event.jhat.which) != 0) hat(event.jhat.value, now, out);
            break;
        case SDL_KEYDOWN:
            if (event.key.repeat == 0) {
                if (auto action = keyboardKey(event.key.keysym.sym)) press(*action, now, out);
            }
            break;
        case SDL_KEYUP:
            if (auto action = keyboardKey(event.key.keysym.sym)) release(*action);
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) repeater_.releaseAll();
            break;
        default:
            break;
    }
}

void SdlInput::update(double now, std::vector<Action>& out) {
    for (Action action : repeater_.update(now)) out.push_back(action);
}

}  // namespace akeno::ui::sdl
