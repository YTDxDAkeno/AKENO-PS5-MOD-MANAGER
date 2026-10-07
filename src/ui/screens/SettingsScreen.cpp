// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

namespace {

constexpr int kRowHeight = 68;
constexpr int kRowGap = 10;
constexpr int kVisibleRows = 10;

const char* sortName(database::LibrarySort sort) {
    switch (sort) {
        case database::LibrarySort::Name: return "Name";
        case database::LibrarySort::TitleId: return "Title ID";
        case database::LibrarySort::RecentlyPlayed: return "Recently played";
    }
    return "Name";
}

std::string itemLabel(SettingsScreen::Item item) {
    switch (item) {
        case SettingsScreen::Item::ShowPs4: return "Show PS4 games";
        case SettingsScreen::Item::ShowHomebrew: return "Show homebrew titles";
        case SettingsScreen::Item::LibrarySort: return "Sort games by";
        case SettingsScreen::Item::DebugLogging: return "Detailed (debug) logging";
        case SettingsScreen::Item::ShadowMountPort: return "ShadowMountPlus API port (127.0.0.1)";
        case SettingsScreen::Item::RunSystemCheck: return "Run the system check";
        case SettingsScreen::Item::ViewLog: return "View the log";
        case SettingsScreen::Item::ExportDiagnostics: return "Export diagnostic log";
        case SettingsScreen::Item::FirstRunGuide: return "Show the first-run guide again";
        case SettingsScreen::Item::Exit: return "Exit Akeno Mod Manager";
    }
    return "";
}

std::string itemValue(SettingsScreen::Item item, const database::Settings& s) {
    switch (item) {
        case SettingsScreen::Item::ShowPs4: return s.showPs4Games ? "On" : "Off";
        case SettingsScreen::Item::ShowHomebrew: return s.showHomebrew ? "On" : "Off";
        case SettingsScreen::Item::LibrarySort: return sortName(s.librarySort);
        case SettingsScreen::Item::DebugLogging: return s.debugLogging ? "On" : "Off";
        case SettingsScreen::Item::ShadowMountPort: return std::to_string(s.shadowMountPort);
        default: return "";
    }
}

}  // namespace

void SettingsScreen::save(UiEnv& env, const database::Settings& settings, const std::string& message) {
    auto saved = env.commands.saveSettings(settings);
    if (saved) {
        env.showToast(message, ToastKind::Success);
    } else {
        env.showToast("Could not save: " + saved.error().message, ToastKind::Error);
    }
}

NavRequest SettingsScreen::handle(Action action, UiEnv& env) {
    list_.setCount(kItemCount);
    list_.setVisibleRows(kVisibleRows);
    if (list_.handle(action)) {
        return NavRequest::none();
    }
    const auto item = static_cast<Item>(list_.focus());
    database::Settings s = env.state.settings;

    if (item == Item::ShadowMountPort && (action == Action::Left || action == Action::Right)) {
        int port = s.shadowMountPort + (action == Action::Right ? 1 : -1);
        if (port < 1) port = 65535;
        if (port > 65535) port = 1;
        s.shadowMountPort = port;
        save(env, s, strings::concat("Port set to ", port, ". Restart Akeno to apply."));
        return NavRequest::none();
    }
    if (action != Action::Confirm) {
        return NavRequest::none();
    }
    switch (item) {
        case Item::ShowPs4:
            s.showPs4Games = !s.showPs4Games;
            save(env, s, s.showPs4Games ? "PS4 games are shown" : "PS4 games are hidden");
            break;
        case Item::ShowHomebrew:
            s.showHomebrew = !s.showHomebrew;
            save(env, s, s.showHomebrew ? "Homebrew titles are shown" : "Homebrew titles are hidden");
            break;
        case Item::LibrarySort:
            s.librarySort = s.librarySort == database::LibrarySort::Name      ? database::LibrarySort::TitleId
                            : s.librarySort == database::LibrarySort::TitleId ? database::LibrarySort::RecentlyPlayed
                                                                              : database::LibrarySort::Name;
            save(env, s, strings::concat("Games sorted by ", sortName(s.librarySort)));
            break;
        case Item::DebugLogging:
            s.debugLogging = !s.debugLogging;
            save(env, s, s.debugLogging ? "Debug logging on" : "Debug logging off");
            break;
        case Item::ShadowMountPort:
            env.showToast("Use LEFT and RIGHT to change the port.");
            break;
        case Item::RunSystemCheck:
            env.commands.runSystemCheck();
            return NavRequest::push(std::make_unique<SystemCheckScreen>());
        case Item::ViewLog:
            return NavRequest::push(std::make_unique<LogViewerScreen>());
        case Item::ExportDiagnostics: {
            auto exported = env.commands.exportDiagnostics();
            if (exported) {
                env.showToast("Written to " + exported.value(), ToastKind::Success);
            } else {
                env.showToast("Export failed: " + exported.error().message, ToastKind::Error);
            }
            break;
        }
        case Item::FirstRunGuide:
            return NavRequest::push(std::make_unique<WizardScreen>());
        case Item::Exit:
            env.commands.requestQuit();
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> SettingsScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L1R1, "Switch tab"}, {ButtonHint::Button::Cross, "Change / open"}};
}

void SettingsScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    list_.setCount(kItemCount);
    list_.setVisibleRows(kVisibleRows);
    const int width = 1200;
    for (int i = 0; i < kItemCount; ++i) {
        const auto item = static_cast<Item>(i);
        const Rect row{content.x, content.y + i * (kRowHeight + kRowGap), width, kRowHeight};
        draw::button(canvas, row, itemLabel(item), list_.focus() == i, true, itemValue(item, env.state.settings));
    }
    const Rect side{content.x + width + 50, content.y, content.w - width - 50, 420};
    draw::panel(canvas, side);
    std::string explanation;
    switch (static_cast<Item>(list_.focus())) {
        case Item::ShowPs4: explanation = "PS4 titles are listed for completeness. Mods target PS5 games first."; break;
        case Item::ShowHomebrew: explanation = "Homebrew apps registered by ShadowMountPlus (LAPY/FAKE IDs)."; break;
        case Item::LibrarySort: explanation = "Order of the Games tab. TRIANGLE in the Games tab does the same."; break;
        case Item::DebugLogging: explanation = "Writes more detail to logs/akeno.log. Secrets are always removed."; break;
        case Item::ShadowMountPort:
            explanation = "Only change this if you changed api_port in ShadowMountPlus. Akeno always connects to "
                          "this console (127.0.0.1) and never over the network.";
            break;
        case Item::RunSystemCheck: explanation = "Checks ShadowMountPlus, storage, network and the database again."; break;
        case Item::ViewLog: explanation = "Recent messages, newest at the bottom."; break;
        case Item::ExportDiagnostics:
            explanation = "Writes a report to the logs folder of the data directory for bug reports.";
            break;
        case Item::FirstRunGuide: explanation = "Walks through the environment check again."; break;
        case Item::Exit: explanation = "Closes Akeno and returns to the console."; break;
    }
    drawWrappedText(canvas, explanation, {side.x + 30, side.y + 30, side.w - 60, side.h - 60},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false}, 40, 9);
    if (!env.state.settingsPersistent) {
        canvas.drawText("The database is unavailable: changes cannot be saved.",
                        {side.x, side.bottom() + 20, side.w, 40},
                        TextStyle{FontRole::Caption, theme::kError, TextAlign::Left, false});
    }
}

}  // namespace akeno::ui
