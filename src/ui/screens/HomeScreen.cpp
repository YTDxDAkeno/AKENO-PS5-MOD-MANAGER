// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

namespace {

enum class HomeAction { ResolveRecovery, BrowseGames, DiscoverMods, SystemCheck, ViewLog, Settings };

std::vector<HomeAction> homeActions(const AppViewState& state) {
    std::vector<HomeAction> actions;
    if (state.recovery) actions.push_back(HomeAction::ResolveRecovery);
    actions.insert(actions.end(),
                   {HomeAction::BrowseGames, HomeAction::DiscoverMods, HomeAction::SystemCheck, HomeAction::ViewLog,
                    HomeAction::Settings});
    return actions;
}

const char* label(HomeAction action) {
    switch (action) {
        case HomeAction::ResolveRecovery: return "Resolve the interrupted operation";
        case HomeAction::BrowseGames: return "Browse installed games";
        case HomeAction::DiscoverMods: return "Discover mods";
        case HomeAction::SystemCheck: return "Run the system check again";
        case HomeAction::ViewLog: return "View the log";
        case HomeAction::Settings: return "Settings";
    }
    return "";
}

struct StatusLine {
    std::string label;
    std::string value;
    Color color;
};

std::vector<StatusLine> statusLines(const AppViewState& state) {
    std::vector<StatusLine> lines;
    const auto& check = state.systemCheck;
    if (!check.report) {
        lines.push_back({"System check", check.running ? "running..." : "not run yet", theme::kNeutral});
    } else {
        const auto& report = *check.report;
        lines.push_back({"Safe Mode", report.features.safeMode() ? "ON - installation disabled" : "OFF",
                         report.features.safeMode() ? theme::kWarning : theme::kOk});
        auto add = [&](app::CheckId id, const char* label) {
            if (const auto* result = report.find(id)) {
                Color color = theme::kNeutral;
                if (result->status == app::CheckStatus::Ok) color = theme::kOk;
                if (result->status == app::CheckStatus::Warning) color = theme::kWarning;
                if (result->status == app::CheckStatus::Failed) color = theme::kError;
                lines.push_back({label, result->summary, color});
            }
        };
        add(app::CheckId::ShadowMountApi, "ShadowMount");
        add(app::CheckId::WritableStorage, "Storage");
        add(app::CheckId::Networking, "Network");
        add(app::CheckId::Database, "Database");
    }
    const auto& library = state.library;
    std::string games;
    if (library.loading && !library.everLoaded) {
        games = "loading...";
    } else if (library.error && !library.everLoaded) {
        games = "unavailable";
    } else {
        games = strings::concat(library.games.size(), library.games.size() == 1 ? " game" : " games");
    }
    lines.push_back({"Installed games", games, theme::kAccent});

    const auto& catalog = state.catalog;
    std::string catalogue;
    Color catalogueColor = theme::kAccent;
    if (!catalog.configured) {
        catalogue = "address not valid";
        catalogueColor = theme::kError;
    } else if (catalog.loaded) {
        catalogue = strings::concat(catalog.games.size(), catalog.games.size() == 1 ? " game with mods" : " games with mods");
    } else if (catalog.loading) {
        catalogue = "loading...";
    } else if (catalog.error) {
        catalogue = "unavailable";
        catalogueColor = theme::kWarning;
    } else {
        catalogue = "opens in Discover";
        catalogueColor = theme::kNeutral;
    }
    lines.push_back({"Mod catalogue", catalogue, catalogueColor});
    return lines;
}

}  // namespace

NavRequest HomeScreen::handle(Action action, UiEnv& env) {
    const auto actions = homeActions(env.state);
    actions_.setCount(static_cast<int>(actions.size()));
    actions_.setVisibleRows(static_cast<int>(actions.size()));
    if (actions_.handle(action)) {
        return NavRequest::none();
    }
    if (action != Action::Confirm || actions.empty()) {
        return NavRequest::none();
    }
    switch (actions[static_cast<std::size_t>(actions_.focus())]) {
        case HomeAction::ResolveRecovery: return NavRequest::push(std::make_unique<RecoveryScreen>());
        case HomeAction::BrowseGames: return NavRequest::switchTab(Tab::Games);
        case HomeAction::DiscoverMods: return NavRequest::switchTab(Tab::Discover);
        case HomeAction::SystemCheck:
            env.commands.runSystemCheck();
            return NavRequest::push(std::make_unique<SystemCheckScreen>());
        case HomeAction::ViewLog: return NavRequest::push(std::make_unique<LogViewerScreen>());
        case HomeAction::Settings: return NavRequest::switchTab(Tab::Settings);
    }
    return NavRequest::none();
}

bool HomeScreen::animating(const UiEnv& env) const {
    return env.state.systemCheck.running || (env.state.library.loading && !env.state.library.everLoaded);
}

void HomeScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const auto actions = homeActions(env.state);
    actions_.setCount(static_cast<int>(actions.size()));

    // Welcome panel and actions (left).
    const Rect welcome{content.x, content.y, 1000, 260};
    draw::panel(canvas, welcome);
    canvas.drawText("Akeno PS5 Mod Manager", {welcome.x + 40, welcome.y + 30, welcome.w - 80, 64},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
    canvas.drawText(strings::concat("Version ", env.state.about.version, "  -  experimental alpha"),
                    {welcome.x + 40, welcome.y + 100, welcome.w - 80, 44},
                    TextStyle{FontRole::Body, theme::kAccent, TextAlign::Left, false});
    drawWrappedText(canvas,
                    "Browse your games and the mods available for them. This version never changes game data.",
                    {welcome.x + 40, welcome.y + 152, welcome.w - 80, 90},
                    TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false}, 44, 2);

    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        const Rect row{content.x, welcome.bottom() + 30 + i * 80, 1000, 68};
        draw::button(canvas, row, label(actions[static_cast<std::size_t>(i)]), actions_.focus() == i);
    }

    // Status panel (right).
    const Rect status{content.x + 1060, content.y, content.w - 1060, content.h};
    draw::panel(canvas, status);
    canvas.drawText("Status", {status.x + 36, status.y + 24, status.w - 72, 56},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    int y = status.y + 100;
    for (const auto& line : statusLines(env.state)) {
        canvas.fillCircle(status.x + 48, y + 30, 9, line.color);
        canvas.drawText(line.label, {status.x + 72, y, status.w - 108, 34},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
        canvas.drawText(line.value, {status.x + 72, y + 34, status.w - 108, 40},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false});
        y += 86;
        if (y > status.bottom() - 80) break;
    }
    if (env.state.systemCheck.running) {
        draw::spinner(canvas, status.right() - 60, status.y + 52, env.time);
    }
}

}  // namespace akeno::ui
