// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

void InstalledModsScreen::update(UiEnv& env) {
    bool confirmed = false;
    if (confirmRemove_.take(confirmed) && confirmed) {
        env.commands.removeInstalledMod(pending_.titleId, pending_.downloadId);
    }
    if (confirmVanilla_.take(confirmed) && confirmed) {
        env.commands.setGameVanilla(pending_.titleId);
    }
}

NavRequest InstalledModsScreen::handle(Action action, UiEnv& env) {
    const std::vector<InstalledModRow> rows = env.commands.listInstalledMods();
    list_.setCount(static_cast<int>(rows.size()));
    list_.setVisibleRows(kVisibleRows);
    if (list_.handle(action) || rows.empty()) return NavRequest::none();
    const InstalledModRow& row = rows[static_cast<std::size_t>(list_.focus())];
    if (env.commands.installBusy() &&
        (action == Action::Confirm || action == Action::Tertiary || action == Action::Secondary)) {
        env.showToast("A mod change is still running. Wait a moment.", ToastKind::Warning);
        return NavRequest::none();
    }
    if (env.commands.installBusy() && action == Action::Options) {
        env.showToast("A mod change is still running. Wait a moment.", ToastKind::Warning);
        return NavRequest::none();
    }
    switch (action) {
        case Action::Options:
            if (!row.test) {
                env.showToast("Only test installs take a test result.");
                break;
            }
            return NavRequest::push(std::make_unique<TestResultScreen>(row));
        case Action::Confirm:
            if (!row.enabled && !row.note.empty()) {
                env.showToast(row.note, ToastKind::Warning);
                break;
            }
            env.commands.setInstalledModEnabled(row.titleId, row.downloadId, !row.enabled);
            env.showToast(row.enabled ? "Turning the mod off..." : "Turning the mod on...");
            break;
        case Action::Tertiary:
            pending_ = row;
            return NavRequest::push(std::make_unique<ConfirmScreen>(
                "Remove this mod?",
                std::vector<std::string>{row.name + " (" + row.gameName + ")",
                                         "It is turned off, the game's overlay is rebuilt without it, and its stored "
                                         "files are deleted. The game's own files are not touched."},
                "Remove", confirmRemove_.callback(), false));
        case Action::Secondary:
            pending_ = row;
            return NavRequest::push(std::make_unique<ConfirmScreen>(
                "Switch " + row.gameName + " to Vanilla?",
                std::vector<std::string>{"Every mod of this game is turned off and Akeno's overlay is removed. The "
                                         "game starts unmodified next time. The mods stay stored."},
                "Vanilla", confirmVanilla_.callback(), false));
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> InstalledModsScreen::hints(const UiEnv& env) const {
    const std::vector<InstalledModRow> rows = env.commands.listInstalledMods();
    if (rows.empty()) return {};
    std::vector<ButtonHint> hints{{ButtonHint::Button::Cross, "On / off"},
                                  {ButtonHint::Button::Square, "Remove"},
                                  {ButtonHint::Button::Triangle, "Vanilla (game)"}};
    const int focus = list_.focus();
    if (focus >= 0 && focus < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(focus)].test) {
        hints.push_back({ButtonHint::Button::Options, "Test result"});
    }
    return hints;
}

void InstalledModsScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const std::vector<InstalledModRow> rows = env.commands.listInstalledMods();
    list_.setCount(static_cast<int>(rows.size()));
    list_.setVisibleRows(kVisibleRows);
    canvas.drawText(strings::concat("Installed mods (", rows.size(), ")"), {content.x, content.y, content.w, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    if (rows.empty()) {
        draw::messagePanel(canvas, {content.x, content.y + 90, content.w, 240}, "No mods installed",
                           "Install a mod from Downloads: open a finished download, check it, then press CROSS.",
                           theme::kNeutral);
        return;
    }
    const int rowHeight = 104;
    for (int i = 0; i < kVisibleRows; ++i) {
        const int index = list_.firstVisible() + i;
        if (index >= static_cast<int>(rows.size())) break;
        const InstalledModRow& row = rows[static_cast<std::size_t>(index)];
        const Rect box{content.x, content.y + 80 + i * (rowHeight + 10), content.w, rowHeight};
        const bool focused = index == list_.focus();
        draw::panel(canvas, box, focused);
        if (focused) canvas.strokeRoundedRect(box, 12, 3, theme::kFocus);
        canvas.fillRoundedRect({box.x, box.y, 10, box.h}, 5, row.enabled ? theme::kOk : theme::kNeutral);
        int nameX = box.x + 30;
        if (row.test) {
            const bool crashed = row.testResult == install::TestResult::Crashed;
            nameX += draw::badge(canvas, nameX, box.y + 12, "TEST", crashed ? theme::kError : theme::kWarning) + 14;
        }
        canvas.drawText(row.name + (row.version.empty() ? std::string() : "  v" + row.version),
                        {nameX, box.y + 10, box.right() - 260 - nameX, 44},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, true});
        std::string detail = strings::concat(row.gameName, "  (", row.titleId, ")   ", strings::formatBytes(row.bytes),
                                             "   from ", row.source);
        Color detailColor = theme::kTextSecondary;
        if (!row.note.empty()) {
            detail = row.note;
            detailColor = theme::kWarning;
        } else if (row.test) {
            detail = "Test in " + row.testFolder + ": " + std::string(install::describe(row.testResult)) +
                     (row.testResult == install::TestResult::Untested ? "  (OPTIONS after playing)" : "");
        }
        canvas.drawText(detail, {box.x + 30, box.y + 56, box.w - 360, 36},
                        TextStyle{FontRole::Caption, detailColor, TextAlign::Left, false});
        canvas.drawText(row.enabled ? "ON" : "OFF", {box.right() - 220, box.y + 10, 200, 44},
                        TextStyle{FontRole::Body, row.enabled ? theme::kOk : theme::kTextDisabled, TextAlign::Right,
                                  true});
        canvas.drawText(row.overlayActive ? "game overlay active" : "game: Vanilla",
                        {box.right() - 320, box.y + 56, 300, 36},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});
    }
}

}  // namespace akeno::ui

namespace akeno::ui {

TestResultScreen::TestResultScreen(InstalledModRow row) : row_(std::move(row)) {
    choices_.setCount(4);
    choices_.setVisibleRows(4);
    choices_.setFocus(3);  // Cancel first
}

NavRequest TestResultScreen::handle(Action action, UiEnv& env) {
    if (action == Action::Back) return NavRequest::pop();
    if (choices_.handle(action)) return NavRequest::none();
    if (action != Action::Confirm) return NavRequest::none();
    static constexpr install::TestResult kResults[] = {install::TestResult::Works, install::TestResult::NoEffect,
                                                       install::TestResult::Crashed};
    const int focus = choices_.focus();
    if (focus >= 0 && focus < 3) {
        if (env.commands.installBusy()) {
            env.showToast("A mod change is still running. Wait a moment.", ToastKind::Warning);
            return NavRequest::none();
        }
        env.commands.reportTestResult(row_.titleId, row_.downloadId, kResults[focus]);
        env.showToast("Saving the test result...");
    }
    return NavRequest::pop();
}

std::vector<ButtonHint> TestResultScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::Circle, "Cancel"}, {ButtonHint::Button::Cross, "Select"}};
}

void TestResultScreen::render(ICanvas& canvas, UiEnv& /*env*/) {
    const Rect box{(theme::kScreenWidth - 1200) / 2, 140, 1200, 780};
    draw::panel(canvas, box, true);
    canvas.fillRoundedRect({box.x, box.y, box.w, 10}, 5, theme::kWarning);
    canvas.drawText("How did the test go?", {box.x + 60, box.y + 40, box.w - 120, 64},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
    const std::vector<std::string> lines{
        row_.name + " (" + row_.gameName + "), test in " + row_.testFolder + ".",
        "Reported so far: " + std::string(install::describe(row_.testResult)) + ". Your answer is kept for this game "
        "version: \"works\" helps later checks, \"crashed\" turns the mod off.",
    };
    int y = box.y + 130;
    for (const auto& line : lines) {
        const int used = drawWrappedText(canvas, line, {box.x + 60, y, box.w - 120, 140},
                                         TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false}, 44, 3);
        y += used * 44 + 20;
    }
    static constexpr const char* kLabels[] = {"It works", "No effect in the game", "The game crashed or did not start",
                                              "Cancel"};
    for (int i = 0; i < 4; ++i) {
        const Rect button{box.x + 60, box.bottom() - 380 + i * 88, box.w - 120, 76};
        draw::button(canvas, button, kLabels[i], choices_.focus() == i);
    }
}

}  // namespace akeno::ui
