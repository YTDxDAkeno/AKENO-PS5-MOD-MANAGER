// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/UiScript.hpp"

#include <algorithm>
#include <charconv>

#include "akeno/core/Strings.hpp"
#include "akeno/security/SafeName.hpp"

namespace akeno::ui {

Result<std::vector<ScriptStep>> parseUiScript(std::string_view text) {
    std::vector<ScriptStep> steps;
    for (const std::string& raw : strings::split(text, ',')) {
        const std::string_view token = strings::trim(raw);
        if (token.empty()) continue;
        ScriptStep step;
        if (strings::startsWith(token, "wait:")) {
            std::string_view value = token.substr(5);
            double seconds = 0.0;
            // std::from_chars for double is not available everywhere; accept integer or one decimal.
            std::size_t dot = value.find('.');
            int whole = 0;
            int tenth = 0;
            auto parse = [](std::string_view digits, int& out) {
                auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
                return ec == std::errc() && ptr == digits.data() + digits.size();
            };
            bool ok = parse(dot == std::string_view::npos ? value : value.substr(0, dot), whole);
            if (dot != std::string_view::npos) ok = ok && value.size() == dot + 2 && parse(value.substr(dot + 1), tenth);
            seconds = whole + tenth / 10.0;
            if (!ok || seconds < 0.0 || seconds > 600.0) {
                return makeError(ErrorCode::InvalidArgument, "Invalid wait in UI script.", std::string(token));
            }
            step.kind = ScriptStep::Kind::Wait;
            step.seconds = seconds;
        } else if (strings::startsWith(token, "shot:")) {
            step.kind = ScriptStep::Kind::Screenshot;
            step.name = std::string(token.substr(5));
            if (!security::isSafeFileComponent(step.name)) {
                return makeError(ErrorCode::InvalidArgument, "Invalid screenshot name in UI script.", step.name);
            }
        } else if (strings::startsWith(token, "type:")) {
            step.kind = ScriptStep::Kind::Type;
            step.name = std::string(token.substr(5));
            const bool printable = std::all_of(step.name.begin(), step.name.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
            if (!printable || step.name.size() > 128) {
                return makeError(ErrorCode::InvalidArgument, "Invalid text in UI script.", step.name);
            }
        } else if (token == "quit") {
            step.kind = ScriptStep::Kind::Quit;
        } else {
            bool found = false;
            for (std::size_t i = 0; i < kActionCount; ++i) {
                auto action = static_cast<Action>(i);
                if (toString(action) == token) {
                    step.kind = ScriptStep::Kind::Press;
                    step.action = action;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return makeError(ErrorCode::InvalidArgument, "Unknown step in UI script.", std::string(token));
            }
        }
        steps.push_back(std::move(step));
        if (steps.size() > 500) {
            return makeError(ErrorCode::InvalidArgument, "UI script is too long.");
        }
    }
    return steps;
}

UiScriptRunner::Tick UiScriptRunner::update(double now) {
    Tick tick;
    if (readyAt_ < 0.0) readyAt_ = now;
    while (next_ < steps_.size() && now >= readyAt_) {
        const ScriptStep& step = steps_[next_++];
        switch (step.kind) {
            case ScriptStep::Kind::Wait:
                readyAt_ = now + step.seconds;
                break;
            case ScriptStep::Kind::Press:
                tick.actions.push_back(step.action);
                // One action per frame so each one is processed and rendered.
                readyAt_ = now + 0.05;
                break;
            case ScriptStep::Kind::Screenshot:
                tick.screenshots.push_back(step.name);
                readyAt_ = now + 0.05;
                break;
            case ScriptStep::Kind::Type:
                tick.typed.push_back(step.name);
                readyAt_ = now + 0.05;
                break;
            case ScriptStep::Kind::Quit:
                tick.quit = true;
                next_ = steps_.size();
                break;
        }
        if (!tick.actions.empty() || !tick.screenshots.empty() || !tick.typed.empty()) break;
    }
    return tick;
}

}  // namespace akeno::ui
