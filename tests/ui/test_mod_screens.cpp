// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "UiTestSupport.hpp"
#include "akeno/mods/ModCheck.hpp"
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
    CHECK(canvas.hasText("The system check is still running."));  // downloads wait for it
    CHECK(h.commands.remoteImages.back() == "https://catalog.example/akeno/shots/1.png");

    detail->handle(Action::Confirm, h.env);  // Download
    CHECK(h.toasted("The system check is still running."));
    CHECK(h.commands.downloadStarts.empty());

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

namespace {

app::SystemReport reportWithDownloads(bool networkOk) {
    app::SystemReport report;
    report.checks = {
        {app::CheckId::Networking, "Networking", networkOk ? app::CheckStatus::Ok : app::CheckStatus::Failed,
         networkOk ? "OK" : "no connection", ""},
        {app::CheckId::WritableStorage, "Storage", app::CheckStatus::Ok, "OK", ""},
    };
    app::BuildFeatures build;
    build.modBrowsing = true;
    build.downloading = true;
    report.features = app::computeFeatures(report.checks, build);
    return report;
}

downloads::DownloadInfo downloadOf(const providers::ModDetails& d, downloads::DownloadState state,
                                   std::uint64_t done, std::string id = "0123456789abcdef") {
    downloads::DownloadInfo info;
    info.record.id = std::move(id);
    info.record.request.mod = d.summary.ref;
    info.record.request.displayName = d.summary.name;
    info.record.request.modVersion = d.summary.version;
    info.record.request.gameTitleId = "PPSA90001";
    info.record.request.compatibility = "VERIFIED";
    info.record.request.expectedSize = 1000;
    info.record.state = state;
    info.record.bytesDone = done;
    return info;
}

}  // namespace

TEST_CASE("the download button follows the compatibility rules and the system check") {
    test::UiHarness h;
    const providers::ModRef ref{"akeno-catalogue", "example-blade/crimson-outfit"};
    ModDetailScreen detail(ref, "Example Blade", providers::GameContext{"PPSA90001", "01.011.000"});
    detail.update(h.env);
    h.state.modDetail.loading = false;
    h.state.modDetail.details = details(true);
    test::RecordingCanvas canvas;

    h.state.systemCheck.report = reportWithDownloads(false);
    detail.render(canvas, h.env);
    CHECK(canvas.hasText("Downloading is not available"));
    detail.handle(Action::Confirm, h.env);
    CHECK(h.commands.downloadStarts.empty());

    h.state.systemCheck.report = reportWithDownloads(true);
    canvas.clear();
    detail.render(canvas, h.env);
    CHECK(canvas.hasText("Download"));
    CHECK(canvas.hasText("Downloads are checked with SHA-256. Nothing is installed."));
    detail.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.downloadStarts.size() == 1);
    CHECK(h.commands.downloadStarts[0].ref == ref);
    CHECK_FALSE(h.commands.downloadStarts[0].confirmed);

    // Once it is in the list, the button shows its state.
    h.state.downloads.items = {downloadOf(*h.state.modDetail.details, downloads::DownloadState::Downloading, 450)};
    canvas.clear();
    detail.render(canvas, h.env);
    CHECK(canvas.hasText("Downloading 45%"));
    detail.handle(Action::Confirm, h.env);
    CHECK(h.commands.downloadStarts.size() == 1);
    CHECK(h.toasted("Already in the Downloads tab."));

    h.state.downloads.items = {downloadOf(*h.state.modDetail.details, downloads::DownloadState::Completed, 1000)};
    canvas.clear();
    detail.render(canvas, h.env);
    CHECK(canvas.hasText("Downloaded"));
    CHECK(canvas.hasText("SHA-256 checked. Installing comes later."));

    h.state.downloads.items = {downloadOf(*h.state.modDetail.details, downloads::DownloadState::Failed, 0)};
    detail.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.resumed.size() == 1);

    // PC-only mods are never downloaded for installing.
    h.state.downloads.items.clear();
    h.state.modDetail.details = details(false);
    detail.handle(Action::Confirm, h.env);
    CHECK(h.commands.downloadStarts.size() == 1);
    CHECK(h.toasted("Akeno will not install this mod"));
}

TEST_CASE("experimental mods need a deliberate confirmation") {
    test::UiHarness h;
    h.state.systemCheck.report = reportWithDownloads(true);
    const providers::ModRef ref{"akeno-catalogue", "example-blade/sharper-foliage"};
    ModDetailScreen detail(ref, "Example Blade", providers::GameContext{"PPSA90001", "01.011.000"});
    detail.update(h.env);
    auto d = details(true);
    d.summary.ref = ref;
    d.summary.compatibility = CompatibilityStatus::Experimental;
    d.needsConfirmation = true;
    d.compatibilityReasons = {"Verified for game version 1.010; installed version is 1.011."};
    h.state.modDetail.loading = false;
    h.state.modDetail.details = d;

    auto nav = detail.handle(Action::Confirm, h.env);
    auto* confirm = dynamic_cast<ConfirmScreen*>(nav.screen.get());
    REQUIRE(confirm != nullptr);
    test::RecordingCanvas canvas;
    confirm->render(canvas, h.env);
    CHECK(canvas.hasText("Download an EXPERIMENTAL mod?"));
    CHECK(canvas.hasText("Verified for game version 1.010; installed version is 1.011."));
    CHECK(canvas.hasText("Download anyway"));
    CHECK(h.commands.downloadStarts.empty());

    // Cancel is focused first.
    CHECK(confirm->handle(Action::Confirm, h.env).kind == NavRequest::Kind::Pop);
    detail.update(h.env);
    CHECK(h.commands.downloadStarts.empty());

    nav = detail.handle(Action::Confirm, h.env);
    confirm = dynamic_cast<ConfirmScreen*>(nav.screen.get());
    REQUIRE(confirm != nullptr);
    confirm->handle(Action::Up, h.env);
    confirm->handle(Action::Confirm, h.env);
    detail.update(h.env);
    REQUIRE(h.commands.downloadStarts.size() == 1);
    CHECK(h.commands.downloadStarts[0].confirmed);

    // Closing the dialog any other way counts as cancel.
    nav = detail.handle(Action::Confirm, h.env);
    nav.screen.reset();
    detail.update(h.env);
    CHECK(h.commands.downloadStarts.size() == 1);
}

TEST_CASE("the Downloads tab shows progress and controls each download") {
    test::UiHarness h;
    h.state.systemCheck.report = reportWithDownloads(true);
    auto d = details(true);
    auto older = downloadOf(d, downloads::DownloadState::Completed, 1000, "aaaaaaaaaaaaaaaa");
    auto newer = downloadOf(d, downloads::DownloadState::Downloading, 250, "bbbbbbbbbbbbbbbb");
    newer.record.request.displayName = "Sharper Foliage Textures";
    newer.progress.bytesPerSecond = 100;
    newer.progress.secondsLeft = 7.5;
    h.state.downloads.items = {older, newer};
    h.state.downloads.freeBytes = 29'400'000'000ull;
    h.state.downloads.reserveBytes = limits::kStorageSafetyReserveBytes;

    ScreenHost host;
    host.setTabRoot(Tab::Downloads, std::make_unique<DownloadsScreen>());
    host.switchTab(Tab::Downloads);
    test::RecordingCanvas canvas;
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Free space 29.4 GB"));
    CHECK(canvas.hasText("Sharper Foliage Textures  v1.2.0"));
    CHECK(canvas.hasText("250 B of 1.0 KB   100 B/s   about 8 s left"));
    CHECK(canvas.hasText("DOWNLOADING"));
    CHECK(canvas.hasText("DOWNLOADED"));
    CHECK(canvas.hasText("SHA-256 checked. Press X to check what is inside."));
    // Newest first: the active one is focused.
    host.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.paused.size() == 1);
    CHECK(h.commands.paused[0] == "bbbbbbbbbbbbbbbb");

    h.state.downloads.items[1].record.state = downloads::DownloadState::Paused;
    host.handle(Action::Confirm, h.env);
    REQUIRE(h.commands.resumed.size() == 1);

    h.state.downloads.items[1].record.state = downloads::DownloadState::Failed;
    h.state.downloads.items[1].record.error = "The server refused the download (HTTP 404).";
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("FAILED"));
    CHECK(canvas.hasText("The server refused the download (HTTP 404)."));

    // Remove asks first.
    host.handle(Action::Tertiary, h.env);
    REQUIRE(host.tabDepth(Tab::Downloads) == 2);
    CHECK(dynamic_cast<ConfirmScreen*>(host.top()) != nullptr);
    host.handle(Action::Up, h.env);      // "Remove"
    host.handle(Action::Confirm, h.env);
    CHECK(host.tabDepth(Tab::Downloads) == 1);
    canvas.clear();
    host.render(canvas, h.env);          // update() picks up the answer
    REQUIRE(h.commands.removed.size() == 1);
    CHECK(h.commands.removed[0] == "bbbbbbbbbbbbbbbb");
    CHECK(h.state.downloads.items.size() == 1);
}

namespace {

mods::ModCheckReport sampleReport(bool blocked) {
    mods::ModCheckReport report;
    report.downloadId = "aaaaaaaaaaaaaaaa";
    report.displayName = "Crimson Outfit Recolour";
    report.modVersion = "1.2.0";
    report.titleId = "PPSA90001";
    report.checkedAt = "2026-10-07T12:00:00Z";
    report.archiveFiles = 3;
    mods::AnalysisInput input;
    input.titleId = "PPSA90001";
    input.catalogueStatus = CompatibilityStatus::Verified;
    input.catalogueInstallable = true;
    archives::ExtractedFile pak;
    pak.path = "Content/Paks/~mods/crimson.pak";
    pak.size = 4000;
    pak.head = "PAK";
    input.files.push_back(pak);
    if (blocked) {
        archives::ExtractedFile dll;
        dll.path = "Binaries/dwmapi.dll";
        dll.size = 100;
        dll.head = "MZ";
        input.files.push_back(dll);
    }
    report.analysis = mods::analyzeMod(input);
    report.plan = mods::planInstall(report.analysis, AppPaths{"/data/akeno-mod-manager"}, "PPSA90001", "aaaaaaaaaaaaaaaa");
    report.conflicts = {{"bbbbbbbbbbbbbbbb", "Another Outfit", {"Content/Paks/~mods/crimson.pak"}, 1}};
    return report;
}

}  // namespace

TEST_CASE("completed downloads open their check, which shows findings, conflicts and the plan") {
    test::UiHarness h;
    auto d = details(true);
    h.state.downloads.items = {downloadOf(d, downloads::DownloadState::Completed, 1000, "aaaaaaaaaaaaaaaa")};
    ScreenHost host;
    host.setTabRoot(Tab::Downloads, std::make_unique<DownloadsScreen>());
    host.switchTab(Tab::Downloads);
    host.handle(Action::Confirm, h.env);
    auto* screen = dynamic_cast<ModCheckScreen*>(host.top());
    REQUIRE(screen != nullptr);

    test::RecordingCanvas canvas;
    host.render(canvas, h.env);  // starts the check
    REQUIRE(h.commands.checks.size() == 1);
    CHECK(h.commands.checks[0] == std::make_pair(std::string("aaaaaaaaaaaaaaaa"), false));
    CHECK(canvas.hasText("Reading the archive"));
    CHECK(canvas.hasText("staging folder"));
    host.handle(Action::Back, h.env);  // cancels instead of leaving
    CHECK(h.commands.checkCancels == 1);
    CHECK(host.tabDepth(Tab::Downloads) == 2);

    h.state.check.running = false;
    h.state.check.report = sampleReport(false);
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(h.commands.checks.size() == 1);
    CHECK(canvas.hasText("NO PROBLEMS FOUND"));
    CHECK(canvas.hasText("VERIFIED"));
    CHECK(canvas.hasText("1 file to install (4.0 KB) from 3 in the archive"));
    CHECK(canvas.hasText("Another Outfit: 1 file in common"));
    CHECK(canvas.hasText("Install plan (dry run: nothing is changed)"));
    for (int i = 0; i < 2; ++i) host.handle(Action::PageDown, h.env);
    CHECK(screen->scroll() > 0);
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Not carried out: Installing is not implemented in this version (Phase 5)."));
    CHECK(canvas.hasText("Content/Paks/~mods/crimson.pak"));

    host.handle(Action::Secondary, h.env);  // check again
    REQUIRE(h.commands.checks.size() == 2);
    CHECK(h.commands.checks[1].second);

    h.state.check.running = false;
    h.state.check.report = sampleReport(true);
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("BLOCKED"));
    CHECK(canvas.hasText("PC ONLY"));
    CHECK(canvas.hasText("Contains Windows programs or libraries (1 files). This is a PC mod. (Binaries/dwmapi.dll)"));

    h.state.check.report.reset();
    h.state.check.error = makeError(ErrorCode::SafetyViolation, "The archive was refused: it contains a symbolic link.");
    canvas.clear();
    host.render(canvas, h.env);
    CHECK(canvas.hasText("The check failed"));
    host.handle(Action::Back, h.env);
    CHECK(host.tabDepth(Tab::Downloads) == 1);
}

TEST_CASE("an empty Downloads tab explains itself, and notices become toasts") {
    test::UiHarness h;
    ScreenHost host;
    host.setTabRoot(Tab::Downloads, std::make_unique<DownloadsScreen>());
    host.switchTab(Tab::Downloads);
    h.state.notices = {{1, "Added to Downloads: Crimson Outfit Recolour", ToastKind::Success}};
    test::RecordingCanvas canvas;
    host.render(canvas, h.env);
    CHECK(canvas.hasText("Nothing downloaded yet"));
    REQUIRE(host.toasts().size() == 1);
    CHECK(host.toasts().front().text == "Added to Downloads: Crimson Outfit Recolour");
    host.render(canvas, h.env);
    CHECK(host.toasts().size() == 1);  // each notice once
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
