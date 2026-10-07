// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "UiTestSupport.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/UiScript.hpp"

using namespace akeno;
using namespace akeno::ui;
using providers::CompatibilityStatus;

namespace {

void loadGames(test::UiHarness& h, std::vector<games::GameInfo> games) {
    h.state.library.everLoaded = true;
    h.state.library.totalGames = games.size();
    h.state.library.games = std::move(games);
}

providers::ProviderGame catalogGame(std::string id, std::string name, std::string titleId, int mods) {
    providers::ProviderGame game;
    game.providerGameId = std::move(id);
    game.name = std::move(name);
    game.titleIds = {std::move(titleId)};
    game.modCount = mods;
    return game;
}

void loadCatalog(test::UiHarness& h) {
    h.state.catalog.configured = true;
    h.state.catalog.source = "https://catalog.example/akeno/";
    h.state.catalog.loaded = true;
    h.state.catalog.games = {catalogGame("neon-harbor", "Neon Harbor", "PPSA90002", 1),
                             catalogGame("example-blade", "Example Blade", "PPSA90001", 6),
                             catalogGame("aurora-drift", "Aurora Drift", "PPSA90003", 2)};
}

providers::ModSummary summary(std::string id, std::string name, CompatibilityStatus status) {
    providers::ModSummary mod;
    mod.ref = {"akeno-catalogue", "example-blade/" + id};
    mod.name = std::move(name);
    mod.author = "Akeno";
    mod.version = "1.2.0";
    mod.shortDescription = "A short description.";
    mod.thumbnailUrl = "https://catalog.example/akeno/thumbs/" + id + ".png";
    mod.compatibility = status;
    mod.downloadSize = 12'300'000;
    mod.updatedAt = "2026-09-01T10:00:00Z";
    return mod;
}

// Answers the pending list request the way the controller would.
void answerList(test::UiHarness& h, std::vector<providers::ModSummary> mods) {
    h.state.modList.loading = false;
    providers::ModPage page;
    page.total = mods.size();
    page.mods = std::move(mods);
    h.state.modList.page = std::move(page);
}

providers::ModDetails details(bool installable) {
    providers::ModDetails d;
    d.summary = summary("crimson-outfit", "Crimson Outfit Recolour", installable ? CompatibilityStatus::Verified
                                                                                 : CompatibilityStatus::PcOnly);
    d.description = "Recolours the outfit.\nWorks with photo mode.";
    d.license = "CC-BY-4.0";
    d.titleIds = {"PPSA90001"};
    d.gameVersions = {"01.011.000"};
    d.modType = "asset-replacement";
    d.files = {{"main", "crimson-outfit-1.2.0.zip", "1.2.0", 12'300'000, std::string(64, 'a'), "zip", true}};
    d.screenshots = {{"https://catalog.example/akeno/shots/1.png", "Front"},
                     {"https://catalog.example/akeno/shots/2.png", "Back"}};
    d.checks = {{"Platform", "PS5", providers::CompatibilityCheck::Mark::Pass},
                {"Game version", "01.011.000 (tested)", providers::CompatibilityCheck::Mark::Pass}};
    d.risk = installable ? "LOW" : "HIGH";
    d.installable = installable;
    if (!installable) d.compatibilityReasons = {"This mod is made for the PC version of the game."};
    return d;
}

}  // namespace

TEST_CASE("Discover loads the catalogue once and lists installed games first") {
    test::UiHarness h;
    h.state.catalog.configured = true;
    h.state.catalog.source = "https://catalog.example/akeno/";
    ScreenHost host;
    host.setTabRoot(Tab::Discover, std::make_unique<DiscoverScreen>());
    host.switchTab(Tab::Discover);
    test::RecordingCanvas canvas;
    host.render(canvas, h.env);
    CHECK(h.commands.catalogLoads == 1);
    CHECK_FALSE(h.commands.lastCatalogForce);
    CHECK(canvas.hasText("Loading the Akeno Catalogue"));
    CHECK(canvas.hasText("catalog.example"));
    host.render(canvas, h.env);
    CHECK(h.commands.catalogLoads == 1);  // still loading: no second request

    loadCatalog(h);
    h.state.catalog.loading = false;
    loadGames(h, {test::makeGame("PPSA90002", "Neon Harbor", "01.004.000")});
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Neon Harbor"));
    CHECK(canvas.hasText("Installed version 01.004.000"));
    CHECK(canvas.hasText("Not installed"));
    CHECK(canvas.hasText("INSTALLED"));
    // Installed first: Neon Harbor, then the others by name.
    std::size_t neon = 0;
    std::size_t aurora = 0;
    for (std::size_t i = 0; i < canvas.texts.size(); ++i) {
        if (canvas.texts[i] == "Neon Harbor") neon = i;
        if (canvas.texts[i] == "Aurora Drift") aurora = i;
    }
    CHECK(neon < aurora);

    host.handle(Action::Confirm, h.env);
    auto* browser = dynamic_cast<ModBrowserScreen*>(host.top());
    REQUIRE(browser != nullptr);
    CHECK(browser->query().providerGameId == "neon-harbor");
    REQUIRE(browser->query().game.has_value());
    CHECK(browser->query().game->titleId == "PPSA90002");
    CHECK(browser->query().game->version == "01.004.000");
    host.handle(Action::Back, h.env);

    host.handle(Action::Down, h.env);  // Aurora Drift: not installed, no game context
    host.handle(Action::Confirm, h.env);
    browser = dynamic_cast<ModBrowserScreen*>(host.top());
    REQUIRE(browser != nullptr);
    CHECK(browser->query().providerGameId == "aurora-drift");
    CHECK_FALSE(browser->query().game.has_value());
    host.handle(Action::Back, h.env);

    host.handle(Action::Options, h.env);
    CHECK(h.commands.catalogLoads == 2);
    CHECK(h.commands.lastCatalogForce);
}

TEST_CASE("Discover explains catalogue problems") {
    test::UiHarness h;
    DiscoverScreen screen;
    test::RecordingCanvas canvas;
    h.state.catalog.configurationError = makeError(ErrorCode::InvalidArgument, "The catalogue address must be an https:// URL.");
    screen.update(h.env);
    screen.render(canvas, h.env);
    CHECK(h.commands.catalogLoads == 0);
    CHECK(canvas.hasText("The catalogue address is not valid"));

    h.state.catalog.configured = true;
    h.state.catalog.error = makeError(ErrorCode::Network, "Could not connect.");
    h.state.catalog.source = "https://catalog.example/";
    screen.update(h.env);  // first attempt for this address
    h.state.catalog.loading = false;
    canvas.clear();
    screen.update(h.env);
    screen.render(canvas, h.env);
    CHECK(h.commands.catalogLoads == 1);  // no automatic retry loop after a failure
    CHECK(canvas.hasText("Could not load the Akeno Catalogue"));
}

TEST_CASE("mod browser lists mods with compatibility badges") {
    test::UiHarness h;
    ScreenHost host;
    host.setTabRoot(Tab::Discover, std::make_unique<DiscoverScreen>());
    host.switchTab(Tab::Discover);
    loadCatalog(h);
    loadGames(h, {test::makeGame("PPSA90001", "Example Blade", "01.011.000")});
    host.handle(Action::Down, h.env);  // installed first, so Example Blade is first anyway
    host.handle(Action::Up, h.env);
    host.handle(Action::Confirm, h.env);
    auto* browser = dynamic_cast<ModBrowserScreen*>(host.top());
    REQUIRE(browser != nullptr);

    test::RecordingCanvas canvas;
    host.render(canvas, h.env);
    REQUIRE(h.commands.listRequests.size() == 1);
    CHECK(h.commands.listRequests[0].providerGameId == "example-blade");
    CHECK(h.commands.listRequests[0].text.empty());
    CHECK(canvas.hasText("Loading mods"));

    answerList(h, {summary("crimson-outfit", "Crimson Outfit Recolour", CompatibilityStatus::Verified),
                   summary("photo-mode", "Photo Mode Unlocker", CompatibilityStatus::PcOnly),
                   summary("mystery", "Mystery Pack", CompatibilityStatus::Unknown)});
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(h.commands.listRequests.size() == 1);  // no reload for the same query
    CHECK(canvas.hasText("Example Blade - Mods"));
    CHECK(canvas.hasText("Crimson Outfit Recolour"));
    CHECK(canvas.hasText("VERIFIED"));
    CHECK(canvas.hasText("PC ONLY"));
    CHECK(canvas.hasText("UNKNOWN"));
    CHECK(canvas.hasText("by Akeno   v1.2.0   12.3 MB   updated 2026-09-01"));
    CHECK(canvas.hasText("Labels are for your installed version 01.011.000"));
    CHECK(canvas.hasText("1 of 3"));
    REQUIRE(h.commands.remoteImages.size() >= 3);
    CHECK(h.commands.remoteImages[0] == "https://catalog.example/akeno/thumbs/crimson-outfit.png");

    host.handle(Action::Down, h.env);
    host.handle(Action::Confirm, h.env);
    auto* detail = dynamic_cast<ModDetailScreen*>(host.top());
    REQUIRE(detail != nullptr);
    CHECK(detail->ref().modId == "example-blade/photo-mode");
}

TEST_CASE("mod browser order and search") {
    test::UiHarness h;
    ModBrowserScreen screen("example-blade", "Example Blade", providers::GameContext{"PPSA90001", "01.011.000"});
    test::RecordingCanvas canvas;
    screen.update(h.env);
    REQUIRE(h.commands.listRequests.size() == 1);
    answerList(h, {summary("crimson-outfit", "Crimson Outfit Recolour", CompatibilityStatus::Verified)});

    screen.handle(Action::Secondary, h.env);
    REQUIRE(h.commands.listRequests.size() == 2);
    CHECK(h.commands.listRequests[1].order == providers::BrowseOrder::Newest);
    CHECK(h.toasted("Order: Newest"));

    screen.handle(Action::Tertiary, h.env);
    CHECK(h.state.textEntry.active);
    CHECK(h.commands.textPrompt == "Search mods for Example Blade");
    h.commands.completeText(std::nullopt);  // cancelled
    screen.update(h.env);
    CHECK(h.commands.listRequests.size() == 2);

    screen.handle(Action::Tertiary, h.env);
    h.commands.completeText(std::string("hair"));
    screen.update(h.env);
    REQUIRE(h.commands.listRequests.size() == 3);
    CHECK(h.commands.listRequests[2].text == "hair");
    CHECK(h.commands.listRequests[2].order == providers::BrowseOrder::Newest);

    answerList(h, {});
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("No mod matches \"hair\""));
    CHECK(canvas.hasText("Search: \"hair\""));
    bool clearHint = false;
    for (const auto& hint : screen.hints(h.env)) clearHint = clearHint || hint.label == "Clear search";
    CHECK(clearHint);

    screen.handle(Action::Options, h.env);
    REQUIRE(h.commands.listRequests.size() == 4);
    CHECK(h.commands.listRequests[3].text.empty());
}

TEST_CASE("mod browser explains missing mods and uninstalled games") {
    test::UiHarness h;
    ModBrowserScreen screen("aurora-drift", "Aurora Drift", std::nullopt);
    screen.update(h.env);
    h.state.modList.loading = false;
    h.state.modList.error = makeError(ErrorCode::NotFound, "The Akeno Catalogue has no mods for this game yet.");
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("No mods for this game yet"));
    CHECK(canvas.hasText("This game is not installed"));
}

TEST_CASE("a text result arriving after the screen closed is harmless") {
    test::UiHarness h;
    {
        ModBrowserScreen screen("example-blade", "Example Blade", std::nullopt);
        screen.handle(Action::Tertiary, h.env);
    }
    h.commands.completeText(std::string("late"));
    CHECK_FALSE(h.state.textEntry.active);
}

TEST_CASE("mod details show compatibility and never pretend to install") {
    test::UiHarness h;
    const providers::ModRef ref{"akeno-catalogue", "example-blade/crimson-outfit"};
    auto screen = std::make_unique<ModDetailScreen>(ref, "Example Blade",
                                                    providers::GameContext{"PPSA90001", "01.011.000"});
    ModDetailScreen* detail = screen.get();
    test::RecordingCanvas canvas;
    detail->update(h.env);
    REQUIRE(h.commands.detailRequests.size() == 1);
    CHECK(h.commands.detailRequests[0] == ref);
    detail->render(canvas, h.env);
    CHECK(canvas.hasText("Loading mod details"));

    h.state.modDetail.loading = false;
    h.state.modDetail.details = details(true);
    canvas.clear();
    detail->update(h.env);
    CHECK(h.commands.detailRequests.size() == 1);
    detail->render(canvas, h.env);
    CHECK(canvas.hasText("Crimson Outfit Recolour"));
    CHECK(canvas.hasText("VERIFIED"));
    CHECK(canvas.hasText("RISK: LOW"));
    CHECK(canvas.hasText("Platform: PS5"));
    CHECK(canvas.hasText("Game version: 01.011.000 (tested)"));
    CHECK(canvas.hasText("Recolours the outfit."));
    CHECK(canvas.hasText("Works with photo mode."));
    CHECK(canvas.hasText("Screenshots (2)"));
    CHECK(canvas.hasText("Downloading is not available in this version."));
    CHECK(h.commands.remoteImages.back() == "https://catalog.example/akeno/shots/1.png");

    detail->handle(Action::Confirm, h.env);  // Download & install
    CHECK(h.toasted("Nothing on your console was changed"));

    detail->handle(Action::Down, h.env);
    auto nav = detail->handle(Action::Confirm, h.env);
    auto* viewer = dynamic_cast<ScreenshotViewerScreen*>(nav.screen.get());
    REQUIRE(viewer != nullptr);
    CHECK(viewer->fullScreen());

    h.state.modDetail.details = details(false);
    h.toasts.clear();
    detail->handle(Action::Up, h.env);
    detail->handle(Action::Confirm, h.env);
    CHECK(h.toasted("Akeno will not install this mod: This mod is made for the PC version"));
    canvas.clear();
    detail->render(canvas, h.env);
    CHECK(canvas.hasText("PC ONLY"));
    CHECK(canvas.hasText("This mod cannot be installed on PS5."));
}

TEST_CASE("long mod descriptions scroll with L2/R2") {
    test::UiHarness h;
    ModDetailScreen detail({"akeno-catalogue", "example-blade/crimson-outfit"}, "Example Blade", std::nullopt);
    detail.update(h.env);
    auto d = details(true);
    d.description.clear();
    for (int i = 0; i < 60; ++i) d.description += "Paragraph line " + std::to_string(i) + "\n";
    h.state.modDetail.loading = false;
    h.state.modDetail.details = d;
    test::RecordingCanvas canvas;
    detail.render(canvas, h.env);
    CHECK_FALSE(canvas.hasText("Paragraph line 40"));
    for (int i = 0; i < 10; ++i) detail.handle(Action::PageDown, h.env);
    CHECK(detail.scroll() > 0);
    canvas.clear();
    detail.render(canvas, h.env);
    CHECK(canvas.hasText("Paragraph line 59"));
    for (int i = 0; i < 20; ++i) detail.handle(Action::PageUp, h.env);
    CHECK(detail.scroll() == 0);
}

TEST_CASE("screenshot viewer cycles through screenshots") {
    test::UiHarness h;
    ScreenshotViewerScreen viewer(details(true).screenshots, 0);
    test::RecordingCanvas canvas;
    viewer.render(canvas, h.env);
    CHECK(canvas.hasText("Front   1 / 2"));
    CHECK(h.commands.remoteImages.back() == "https://catalog.example/akeno/shots/1.png");
    CHECK(canvas.images.back() == "remote:https://catalog.example/akeno/shots/1.png:1600x900");
    viewer.handle(Action::Right, h.env);
    CHECK(viewer.index() == 1);
    viewer.handle(Action::Right, h.env);
    CHECK(viewer.index() == 0);
    viewer.handle(Action::Left, h.env);
    CHECK(viewer.index() == 1);
    CHECK(viewer.handle(Action::Back, h.env).kind == NavRequest::Kind::Pop);
}

TEST_CASE("game details open the mod browser when the catalogue has the game") {
    test::UiHarness h;
    h.state.systemCheck.report.reset();
    loadGames(h, {test::makeGame("PPSA90001", "Example Blade", "01.011.000"),
                  test::makeGame("PPSA00042", "Other Game", "1.0")});
    loadCatalog(h);
    GameDetailScreen blade("PPSA90001");
    test::RecordingCanvas canvas;
    blade.update(h.env);
    CHECK(h.commands.catalogLoads == 0);  // already loaded
    blade.render(canvas, h.env);
    CHECK(canvas.hasText("Browse mods (6)"));
    auto nav = blade.handle(Action::Confirm, h.env);
    auto* browser = dynamic_cast<ModBrowserScreen*>(nav.screen.get());
    REQUIRE(browser != nullptr);
    CHECK(browser->query().game->version == "01.011.000");

    GameDetailScreen other("PPSA00042");
    canvas.clear();
    other.render(canvas, h.env);
    CHECK(canvas.hasText("No mods in the Akeno Catalogue yet"));
    nav = other.handle(Action::Confirm, h.env);
    CHECK(nav.kind == NavRequest::Kind::None);
    CHECK(h.toasted("has no mods for this game yet"));
}

TEST_CASE("the catalogue address is changed through the keyboard and validated") {
    test::UiHarness h;
    SettingsScreen screen;
    for (int i = 0; i < 5; ++i) screen.handle(Action::Down, h.env);
    test::RecordingCanvas canvas;
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("Mod catalogue address"));
    CHECK(canvas.hasText("Default"));

    screen.handle(Action::Confirm, h.env);
    CHECK(h.state.textEntry.active);
    h.commands.completeText(std::string("http://evil.example/catalog/"));
    screen.update(h.env);
    CHECK(h.commands.saves == 0);
    CHECK(h.toasted("must start with https://"));

    screen.handle(Action::Confirm, h.env);
    h.commands.completeText(std::string("https://mods.example.org/akeno/"));
    screen.update(h.env);
    CHECK(h.commands.saves == 1);
    CHECK(h.state.settings.catalogueUrl == "https://mods.example.org/akeno/");
    canvas.clear();
    screen.render(canvas, h.env);
    CHECK(canvas.hasText("mods.example.org"));

    screen.handle(Action::Confirm, h.env);
    h.commands.completeText(std::string(""));
    screen.update(h.env);
    CHECK(h.commands.saves == 2);
    CHECK(h.state.settings.catalogueUrl == database::kDefaultCatalogueUrl);
    CHECK(h.toasted("Using the default Akeno Catalogue"));
}

TEST_CASE("the text entry overlay blocks navigation while open") {
    test::UiHarness h;
    ScreenHost host;
    host.setTabRoot(Tab::Home, std::make_unique<HomeScreen>());
    h.state.textEntry.active = true;
    h.state.textEntry.prompt = "Search mods for Example Blade";
    h.state.textEntry.text = "hai";
    const Tab before = host.currentTab();
    host.handle(Action::NextTab, h.env);
    CHECK(host.currentTab() == before);
    test::RecordingCanvas canvas;
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Search mods for Example Blade"));
    CHECK(canvas.hasText("Type, then press ENTER. ESC cancels."));
    h.state.textEntry.systemKeyboard = true;
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("console keyboard"));
}

TEST_CASE("UI scripts can type into a text entry") {
    auto steps = parseUiScript("tertiary,type:hair dye,wait:0.5,shot:search");
    REQUIRE(steps.ok());
    REQUIRE(steps->size() == 4);
    CHECK((*steps)[1].kind == ScriptStep::Kind::Type);
    CHECK((*steps)[1].name == "hair dye");
    CHECK_FALSE(parseUiScript("type:bad\x01text").ok());
    CHECK_FALSE(parseUiScript("type:" + std::string(200, 'a')).ok());

    UiScriptRunner runner(std::move(steps).value());
    auto tick = runner.update(0.0);
    CHECK(tick.actions.size() == 1);
    tick = runner.update(0.1);
    REQUIRE(tick.typed.size() == 1);
    CHECK(tick.typed[0] == "hair dye");
}

TEST_CASE("wrapText breaks at spaces and keeps paragraphs") {
    test::RecordingCanvas canvas;  // 15 px per character
    auto lines = wrapText(canvas, "one two three four\n\nfive", 15 * 10, FontRole::Body, false);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "one two");
    CHECK(lines[1] == "three four");
    CHECK(lines[2].empty());
    CHECK(lines[3] == "five");
    auto longWord = wrapText(canvas, "https://example.com/a/very/long/path", 60, FontRole::Body, false);
    REQUIRE(longWord.size() == 1);
}
