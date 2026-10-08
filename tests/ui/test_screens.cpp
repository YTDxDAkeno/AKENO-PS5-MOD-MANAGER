// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "UiTestSupport.hpp"
#include "akeno/core/BuildInfo.hpp"
#include "akeno/ui/Screens.hpp"

using namespace akeno;
using namespace akeno::ui;

namespace {

app::SystemReport reportWith(app::CheckStatus shadowMountApi) {
    app::SystemReport report;
    report.generatedAt = "2026-10-07T00:00:00Z";
    report.checks = {
        {app::CheckId::Firmware, "Firmware", app::CheckStatus::Ok, "12.20", ""},
        {app::CheckId::ShadowMountApi, "ShadowMount API", shadowMountApi,
         shadowMountApi == app::CheckStatus::Ok ? "connected (ShadowMount+ 1.7, API v1)" : "not connected", ""},
        {app::CheckId::Database, "Database", app::CheckStatus::Ok, "OK (schema v1)", ""},
    };
    report.features = app::computeFeatures(report.checks, app::BuildFeatures{});
    return report;
}

void loadGames(test::UiHarness& h, std::vector<games::GameInfo> games) {
    h.state.library.everLoaded = true;
    h.state.library.totalGames = games.size();
    h.state.library.games = std::move(games);
}

}  // namespace

TEST_CASE("startup flow order: recovery, then first-run guide or system check") {
    test::UiHarness h;
    {
        ScreenHost host;
        setupScreens(host, h.state);
        REQUIRE(host.top() != nullptr);
        CHECK(dynamic_cast<WizardScreen*>(host.top()) != nullptr);
    }
    h.state.settings.firstRunComplete = true;
    {
        ScreenHost host;
        setupScreens(host, h.state);
        CHECK(dynamic_cast<SystemCheckScreen*>(host.top()) != nullptr);
    }
    OperationState op;
    op.operationId = "install-1";
    op.kind = "install";
    h.state.recovery = RecoveryView{op, adviseRecovery(op)};
    {
        ScreenHost host;
        setupScreens(host, h.state);
        CHECK(dynamic_cast<RecoveryScreen*>(host.top()) != nullptr);
        CHECK(host.flowDepth() == 2);
    }
}

TEST_CASE("tabs switch with L1/R1 and wrap around") {
    test::UiHarness h;
    h.state.settings.firstRunComplete = true;
    ScreenHost host;
    setupScreens(host, h.state);
    // Close the system check flow (finished check).
    h.state.systemCheck.report = reportWith(app::CheckStatus::Ok);
    host.handle(Action::Confirm, h.env);
    REQUIRE(host.flowDepth() == 0);
    CHECK(host.currentTab() == Tab::Home);
    host.handle(Action::NextTab, h.env);
    CHECK(host.currentTab() == Tab::Games);
    host.handle(Action::PrevTab, h.env);
    host.handle(Action::PrevTab, h.env);
    CHECK(host.currentTab() == Tab::About);
}

TEST_CASE("the system check cannot be dismissed while it runs") {
    test::UiHarness h;
    h.state.settings.firstRunComplete = true;
    ScreenHost host;
    setupScreens(host, h.state);
    h.state.systemCheck.running = true;
    host.handle(Action::Confirm, h.env);
    CHECK(host.flowDepth() == 1);
    h.state.systemCheck.running = false;
    h.state.systemCheck.report = reportWith(app::CheckStatus::Ok);
    test::RecordingCanvas canvas;
    host.render(canvas, h.env);
    CHECK(canvas.hasText("SAFE MODE: ON"));
    CHECK(canvas.hasText("Installation: NOT YET IMPLEMENTED"));
    host.handle(Action::Secondary, h.env);
    CHECK(h.commands.systemChecks == 1);
    host.handle(Action::Confirm, h.env);
    CHECK(host.flowDepth() == 0);
}

TEST_CASE("game library shows games and opens details") {
    test::UiHarness h;
    h.state.systemCheck.report = reportWith(app::CheckStatus::Ok);
    auto blade = test::makeGame("PPSA01234", "Example Blade", "1.011");
    blade.hasIcon = true;
    loadGames(h, {blade, test::makeGame("PPSA05678", "Second Game", "")});

    ScreenHost host;
    host.setTabRoot(Tab::Games, std::make_unique<GameLibraryScreen>());
    host.switchTab(Tab::Games);
    test::RecordingCanvas canvas;
    canvas.availableImages = {"icon:PPSA01234:180"};
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Installed Games (2)"));
    CHECK(canvas.hasText("Example Blade"));
    CHECK(canvas.hasText("PPSA01234"));
    CHECK(canvas.hasText("Version 1.011"));
    CHECK(canvas.hasText("Version unknown"));
    CHECK(canvas.hasText("0 installed mods"));
    REQUIRE_FALSE(canvas.images.empty());
    CHECK(canvas.images[0] == "icon:PPSA01234:180");

    host.handle(Action::Right, h.env);
    host.handle(Action::Confirm, h.env);
    CHECK(host.tabDepth(Tab::Games) == 2);
    auto* detail = dynamic_cast<GameDetailScreen*>(host.top());
    REQUIRE(detail != nullptr);
    CHECK(detail->titleId() == "PPSA05678");

    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Second Game"));
    CHECK(canvas.hasText("Installed mods"));
    host.handle(Action::Confirm, h.env);  // "Browse mods": no catalogue in this harness
    CHECK(h.toasted("catalogue address is not valid"));
    // Vanilla: nothing to do without active mods; with them, it asks first.
    host.handle(Action::Down, h.env);
    host.handle(Action::Confirm, h.env);
    CHECK(h.toasted("No mods are active"));
    h.commands.installedSummaries["PPSA05678"] = InstalledModsSummary{2, 1, true, std::nullopt};
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Vanilla (mods off)"));
    CHECK(canvas.hasText("2 (1 on)"));
    host.handle(Action::Confirm, h.env);
    REQUIRE(dynamic_cast<ConfirmScreen*>(host.top()) != nullptr);
    host.handle(Action::Up, h.env);
    host.handle(Action::Confirm, h.env);
    host.render(canvas, h.env);
    REQUIRE(h.commands.vanillas.size() == 1);
    CHECK(h.commands.vanillas[0] == "PPSA05678");
    host.handle(Action::Up, h.env);
    host.handle(Action::Back, h.env);
    CHECK(host.tabDepth(Tab::Games) == 1);
    host.handle(Action::Back, h.env);  // the tab root stays
    CHECK(host.tabDepth(Tab::Games) == 1);
}

TEST_CASE("game library explains a missing ShadowMountPlus") {
    test::UiHarness h;
    h.state.systemCheck.report = reportWith(app::CheckStatus::Failed);
    GameLibraryScreen screen;
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("The game library is not available"));

    screen.handle(Action::Options, h.env);
    CHECK(h.commands.systemChecks == 1);  // checks for ShadowMountPlus again instead
    CHECK(h.commands.libraryRefreshes == 0);

    h.state.systemCheck.report = reportWith(app::CheckStatus::Ok);
    screen.handle(Action::Options, h.env);
    CHECK(h.commands.libraryRefreshes == 1);
}

TEST_CASE("game library loading, error and empty states") {
    test::UiHarness h;
    GameLibraryScreen screen;
    test::RecordingCanvas canvas;
    h.state.library.loading = true;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Asking ShadowMountPlus"));
    CHECK(screen.animating(h.env));

    canvas.clear();
    h.state.library.loading = false;
    h.state.library.error = makeError(ErrorCode::Timeout, "ShadowMountPlus is not answering on port 10101.");
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Could not read your games"));

    canvas.clear();
    h.state.library.error.reset();
    h.state.library.everLoaded = true;
    h.state.library.totalGames = 3;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("No games found"));
}

TEST_CASE("triangle and square in the library change saved settings") {
    test::UiHarness h;
    loadGames(h, {test::makeGame("PPSA01234", "Example", "1.0")});
    GameLibraryScreen screen;
    screen.handle(Action::Secondary, h.env);
    CHECK(h.state.settings.librarySort == database::LibrarySort::TitleId);
    screen.handle(Action::Tertiary, h.env);
    CHECK_FALSE(h.state.settings.showPs4Games);
    CHECK(h.commands.saves == 2);

    h.commands.failSave = true;
    screen.handle(Action::Tertiary, h.env);
    CHECK(h.toasted("Could not save"));
}

TEST_CASE("settings toggles are saved and exit is requested") {
    test::UiHarness h;
    SettingsScreen screen;
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Show PS4 games"));
    CHECK(canvas.hasText("ShadowMountPlus API port (127.0.0.1)"));

    screen.handle(Action::Confirm, h.env);  // Show PS4 games
    CHECK_FALSE(h.state.settings.showPs4Games);
    for (int i = 0; i < 4; ++i) screen.handle(Action::Down, h.env);  // port
    screen.handle(Action::Right, h.env);
    CHECK(h.state.settings.shadowMountPort == 10102);
    CHECK(h.toasted("Restart Akeno"));
    for (int i = 0; i < 10; ++i) screen.handle(Action::Down, h.env);  // exit (last item)
    screen.handle(Action::Confirm, h.env);
    CHECK(h.commands.quit);
}

TEST_CASE("settings can open the system check and the first-run guide") {
    test::UiHarness h;
    SettingsScreen screen;
    for (int i = 0; i < 6; ++i) screen.handle(Action::Down, h.env);
    // GameBanana is off until switched on, and says what is sent.
    CHECK_FALSE(h.state.settings.gameBanana);
    screen.handle(Action::Confirm, h.env);
    CHECK(h.state.settings.gameBanana);
    CHECK(h.toasted("GameBanana is on"));
    screen.handle(Action::Down, h.env);
    auto nav = screen.handle(Action::Confirm, h.env);
    CHECK(nav.kind == NavRequest::Kind::Push);
    CHECK(dynamic_cast<SystemCheckScreen*>(nav.screen.get()) != nullptr);
    CHECK(h.commands.systemChecks == 1);
    for (int i = 0; i < 3; ++i) screen.handle(Action::Down, h.env);
    nav = screen.handle(Action::Confirm, h.env);
    CHECK(dynamic_cast<WizardScreen*>(nav.screen.get()) != nullptr);
}

TEST_CASE("planned tabs say plainly that the feature is not available") {
    test::UiHarness h;
    ScreenHost host;
    setupScreens(host, h.state);
    for (Tab tab : {Tab::Downloads, Tab::InstalledMods, Tab::Updates}) {
        host.switchTab(tab);
        auto* screen = host.top();
        REQUIRE(screen != nullptr);
    }
    PlannedFeatureScreen screen("Downloads", "Planned for Phase 3", {"line"});
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("NOT AVAILABLE IN THIS VERSION"));
    CHECK(canvas.hasText("Planned for Phase 3"));
}

TEST_CASE("first-run guide walks through the steps and records completion") {
    test::UiHarness h;
    WizardScreen wizard;
    wizard.handle(Action::Confirm, h.env);
    CHECK(wizard.step() == 1);
    CHECK(h.commands.systemChecks == 1);

    h.state.systemCheck.running = true;
    wizard.handle(Action::Confirm, h.env);
    CHECK(wizard.step() == 1);  // waits for the check
    h.state.systemCheck.running = false;
    h.state.systemCheck.report = reportWith(app::CheckStatus::Ok);
    wizard.handle(Action::Confirm, h.env);
    CHECK(wizard.step() == 2);
    wizard.handle(Action::Confirm, h.env);
    CHECK(wizard.step() == 3);
    CHECK(h.commands.libraryRefreshes == 1);
    wizard.handle(Action::Back, h.env);
    CHECK(wizard.step() == 2);
    wizard.handle(Action::Confirm, h.env);
    CHECK(h.commands.libraryRefreshes == 1);  // not requested twice

    test::RecordingCanvas canvas;
    wizard.handle(Action::Confirm, h.env);  // step 4 providers
    wizard.render(canvas, h.env);
    CHECK(canvas.hasText("Step 4 of 5"));
    wizard.handle(Action::Confirm, h.env);  // step 5 vanilla
    wizard.handle(Action::Confirm, h.env);  // ready
    CHECK(wizard.step() == WizardScreen::kStepCount - 1);
    auto nav = wizard.handle(Action::Confirm, h.env);
    CHECK(nav.kind == NavRequest::Kind::Pop);
    CHECK(h.state.settings.firstRunComplete);
}

TEST_CASE("recovery screen cleans staging or postpones") {
    test::UiHarness h;
    OperationState op;
    op.operationId = "install-1";
    op.kind = "install";
    op.stagingPaths = {"/data/akeno-mod-manager/staging/install-1"};
    h.state.recovery = RecoveryView{op, adviseRecovery(op)};

    RecoveryScreen screen;
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Interrupted installation detected."));
    CHECK(canvas.hasText("No active overlay was changed."));
    CHECK(canvas.hasText("Clean up staging data"));
    auto nav = screen.handle(Action::Confirm, h.env);
    CHECK(h.commands.cleanups == 1);
    CHECK(nav.kind == NavRequest::Kind::Pop);

    h.state.recovery = RecoveryView{op, adviseRecovery(op)};
    RecoveryScreen second;
    nav = second.handle(Action::Back, h.env);
    CHECK(h.commands.postpones == 1);
    CHECK(nav.kind == NavRequest::Kind::Pop);
}

TEST_CASE("home offers recovery when an operation is pending") {
    test::UiHarness h;
    HomeScreen home;
    OperationState op;
    op.operationId = "x";
    op.kind = "install";
    h.state.recovery = RecoveryView{op, adviseRecovery(op)};
    test::RecordingCanvas canvas;
    home.render(canvas, h.env);
    CHECK(canvas.hasText("Resolve the interrupted operation"));
    auto nav = home.handle(Action::Confirm, h.env);
    CHECK(dynamic_cast<RecoveryScreen*>(nav.screen.get()) != nullptr);
}

TEST_CASE("log viewer shows records and exports diagnostics") {
    test::UiHarness h;
    logging::LogRecord record;
    record.timestamp = "2026-10-07T11:22:33Z";
    record.category = "startup";
    record.message = "hello";
    h.commands.logs = {record};
    LogViewerScreen screen;
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("11:22:33  [startup] hello"));
    screen.handle(Action::Options, h.env);
    REQUIRE(h.commands.diagnosticTitles.size() == 1);
    CHECK(h.commands.diagnosticTitles.front().empty());
    CHECK(screen.handle(Action::Back, h.env).kind == NavRequest::Kind::Pop);
}

TEST_CASE("about screen states the testing status") {
    test::UiHarness h;
    h.state.about.version = "0.1.0-alpha";
    h.state.about.testingStatus = std::string(build::testingStatus());
    AboutScreen screen;
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    // Wrapped over several lines, but complete: the lines joined give the whole text.
    std::string joined;
    for (const auto& text : canvas.texts) joined += text + " ";
    CHECK(joined.find("Testing status: " + h.state.about.testingStatus) != std::string::npos);
    CHECK_FALSE(canvas.hasText(h.state.about.testingStatus));  // not on a single line
}

TEST_CASE("toasts expire") {
    test::UiHarness h;
    ScreenHost host;
    host.setTabRoot(Tab::Home, std::make_unique<HomeScreen>());
    host.addToast("hello", ToastKind::Info, 0.0);
    CHECK(host.toasts().size() == 1);
    test::RecordingCanvas canvas;
    h.env.time = 10.0;
    host.render(canvas, h.env);
    CHECK(host.toasts().empty());
}

TEST_CASE("installed mods can be turned off, removed, and a game switched to Vanilla") {
    test::UiHarness h;
    InstalledModsScreen screen;
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("No mods installed"));
    h.commands.installedRows = {
        {"PPSA24701", "Digimon", "aaaa1111", "Playable Sayo", "1.0.6", "gamebanana", true, true, 1000},
        {"PPSA24701", "Digimon", "bbbb2222", "Kotone", "1.0.2", "gamebanana", false, true, 2000},
    };
    canvas.clear();
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Installed mods (2)"));
    CHECK(canvas.hasText("ON"));
    screen.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.toggles.size() == 1);
    CHECK(h.commands.toggles[0] == "PPSA24701/aaaa1111:off");

    auto nav = screen.handle(Action::Tertiary, h.env);
    auto* confirm = dynamic_cast<ConfirmScreen*>(nav.screen.get());
    REQUIRE(confirm != nullptr);
    confirm->handle(Action::Up, h.env);
    confirm->handle(Action::Confirm, h.env);
    screen.update(h.env);
    REQUIRE(h.commands.removals.size() == 1);
    CHECK(h.commands.removals[0] == "PPSA24701/aaaa1111");

    screen.handle(Action::Down, h.env);
    nav = screen.handle(Action::Secondary, h.env);
    confirm = dynamic_cast<ConfirmScreen*>(nav.screen.get());
    REQUIRE(confirm != nullptr);
    confirm->handle(Action::Up, h.env);
    confirm->handle(Action::Confirm, h.env);
    screen.update(h.env);
    REQUIRE(h.commands.vanillas.size() == 1);
    CHECK(h.commands.vanillas[0] == "PPSA24701");
}

TEST_CASE("diagnostic export is available per game without enabling mods") {
    test::UiHarness h;
    games::GameInfo game;
    game.titleId = "PPSA24701";
    game.name = "Diagnostic game";
    h.state.library.games.push_back(game);
    GameDetailScreen screen(game.titleId);
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Export Diagnostics"));
    screen.handle(Action::Down, h.env);
    screen.handle(Action::Down, h.env);
    screen.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.diagnosticTitles.size() == 1);
    CHECK(h.commands.diagnosticTitles[0] == game.titleId);
    CHECK(h.commands.vanillas.empty());
    h.state.diagnostics.running = true;
    screen.handle(Action::Confirm, h.env);
    CHECK(h.commands.diagnosticCancels == 1);
}

TEST_CASE("settings diagnostic export starts asynchronously and can be cancelled") {
    test::UiHarness h;
    SettingsScreen screen;
    for (int i = 0; i < static_cast<int>(SettingsScreen::Item::ExportDiagnostics); ++i)
        screen.handle(Action::Down, h.env);
    screen.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.diagnosticTitles.size() == 1);
    CHECK(h.commands.diagnosticTitles[0].empty());
    h.state.diagnostics.running = true;
    screen.handle(Action::Confirm, h.env);
    CHECK(h.commands.diagnosticCancels == 1);
}
