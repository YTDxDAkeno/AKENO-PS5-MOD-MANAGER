// SPDX-License-Identifier: GPL-3.0-or-later
// Scripted input for automated UI checks and screenshots (developer option --ui-script).
//   "wait:2,next-tab,confirm,shot:games,quit"
// Steps: wait:<seconds>, an action name (see toString(Action)), shot:<name>, type:<text>
// (completes an open text entry with <text>; printable ASCII without commas), quit.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/ui/Input.hpp"

namespace akeno::ui {

struct ScriptStep {
    enum class Kind { Wait, Press, Screenshot, Type, Quit };
    Kind kind = Kind::Wait;
    double seconds = 0.0;
    Action action = Action::Confirm;
    std::string name;  // screenshot name (a safe file component) or the text to type
};

Result<std::vector<ScriptStep>> parseUiScript(std::string_view text);

class UiScriptRunner {
public:
    explicit UiScriptRunner(std::vector<ScriptStep> steps) : steps_(std::move(steps)) {}

    struct Tick {
        std::vector<Action> actions;
        std::vector<std::string> screenshots;
        std::vector<std::string> typed;
        bool quit = false;
    };

    // Advances through every step that is due at `now`.
    Tick update(double now);
    bool finished() const { return next_ >= steps_.size(); }

private:
    std::vector<ScriptStep> steps_;
    std::size_t next_ = 0;
    double readyAt_ = -1.0;
};

}  // namespace akeno::ui
