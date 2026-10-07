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
    switch (action) {
        case Action::Confirm:
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
    if (env.commands.listInstalledMods().empty()) return {};
    return {{ButtonHint::Button::Cross, "On / off"},
            {ButtonHint::Button::Square, "Remove"},
            {ButtonHint::Button::Triangle, "Vanilla (game)"}};
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
        canvas.drawText(row.name + (row.version.empty() ? std::string() : "  v" + row.version),
                        {box.x + 30, box.y + 10, box.w - 260, 44},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, true});
        canvas.drawText(strings::concat(row.gameName, "  (", row.titleId, ")   ", strings::formatBytes(row.bytes),
                                        "   from ", row.source),
                        {box.x + 30, box.y + 56, box.w - 260, 36},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
        canvas.drawText(row.enabled ? "ON" : "OFF", {box.right() - 220, box.y + 10, 200, 44},
                        TextStyle{FontRole::Body, row.enabled ? theme::kOk : theme::kTextDisabled, TextAlign::Right,
                                  true});
        canvas.drawText(row.overlayActive ? "game overlay active" : "game: Vanilla",
                        {box.right() - 320, box.y + 56, 300, 36},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});
    }
}

}  // namespace akeno::ui
