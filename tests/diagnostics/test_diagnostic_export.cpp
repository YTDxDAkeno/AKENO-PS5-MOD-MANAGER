// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"
#include "TestSupport.hpp"
#include "akeno/diagnostics/DiagnosticExport.hpp"
#include "akeno/security/Sha256.hpp"
#include <sys/stat.h>
#include <unistd.h>

using namespace akeno;
using json::Json;
namespace fs = std::filesystem;
namespace {
Json readJson(const fs::path& path) {
    auto text = security::readFileBounded(path, 64 * 1024 * 1024);
    REQUIRE(text.ok());
    return Json::parse(*text);
}
struct Fixture {
    test::TempDir dir;
    AppPaths paths{dir.path() / "akeno"};
    fs::path scanRoot = dir.path() / "scan";
    fs::path game = scanRoot / "game";
    fs::path overlay = scanRoot / "backports/PPSA24701";
    security::SafeFs safeFs{security::WriteGuard::create({paths.root}).value()};
    test::MockHttpClient http;
    shadowmount::ShadowMountClient client = shadowmount::ShadowMountClient::create(http, {}).value();
    diagnostics::Request request;
    Fixture() {
        for (const auto& path : paths.layout()) fs::create_directories(path);
        fs::create_directories(game / "sce_sys");
        fs::create_directories(overlay);
        test::writeText(game / "sce_sys/param.json", R"({"titleId":"PPSA24701","contentVersion":"01.011.000"})");
        test::writeText(game / "asset.bin", "copyrighted-game-bytes-not-to-be-exported");
        test::writeText(overlay / "asset.bin", "PC replacement");
        request.paths = paths;
        request.titleId = "PPSA24701";
        request.backportsRoot = overlay.parent_path();
        request.firmware = {true, 0x12200000, "12.20"};
        request.logFiles = {paths.logs() / "akeno.log", dir.path() / "missing.log"};
        test::writeText(request.logFiles[0], "failure token=topsecret\n");
        respond("version", R"({"status":0,"api_version":1,"shadowmount_version":"1.7beta4","capabilities":["list_games"]})");
        respond("settings", Json{{"status", 0}, {"scan_paths", {scanRoot.string()}}, {"scan_path_count", 1},
            {"debug", true}, {"api_key", "do-not-export"}}.dump());
        respond("games", Json{{"status", 0}, {"games", Json::array({{{"title_id", "PPSA24701"},
            {"path", game.string()}, {"platform", "ps5"}, {"source_type", "folder"}, {"installed_pkg", false}}})}}.dump());
    }
    void respond(const std::string& route, std::string body) {
        http.respond(network::HttpMethod::Post, "http://127.0.0.1:10101/api/v1/" + route, 200, std::move(body));
    }
    Json run(const CancellationToken* cancel = nullptr) {
        auto path = diagnostics::exportReport(request, safeFs, client, cancel);
        REQUIRE(path.ok());
        CHECK(security::isWithin(paths.logs(), *path));
        return readJson(*path);
    }
};
}

TEST_CASE("native export inventories physical files without exporting assets or mutating installed state") {
    Fixture f;
    const auto store = f.paths.mods() / "PPSA24701/mod1/files";
    fs::create_directories(store);
    test::writeText(store / "ModConfig.json", R"({"ModDependencies":["DSTS.ModLoader","MVGL.FileLoader.Reloaded"]})");
    const auto hash = security::sha256File(store / "ModConfig.json").value();
    const auto size = fs::file_size(store / "ModConfig.json");
    const auto stateFile = store.parent_path().parent_path() / "state.json";
    Json fileRecord{{"storePath", "ModConfig.json"}, {"installPath", "ModConfig.json"}, {"size", size}, {"sha256", hash}};
    Json modRecord{{"downloadId", "mod1"}, {"provider", "nexus"}, {"enabled", true}, {"files", Json::array({fileRecord})}};
    const auto state = Json{{"schemaVersion", 1}, {"titleId", "PPSA24701"}, {"overlayActive", true},
                            {"mods", Json::array({modRecord})}}.dump();
    test::writeText(stateFile, state);
    test::writeText(f.paths.operationJournal(), R"({"kind":"activate","step":"swap","activeOverlayTouched":true})");
    const auto before = security::sha256File(f.game / "asset.bin").value();
    auto report = f.run();
    REQUIRE(report["titles"].size() == 1);
    const auto& title = report["titles"][0];
    CHECK(title["gameVersion"] == "01.011.000");
    CHECK(title["selection"]["status"] == "predicted-akeno");
    CHECK(title["vanilla"]["complete"] == true);
    CHECK(title["overlay"]["entries"][0]["gamePathCheck"] == "existing-same-type");
    CHECK(title["overlay"]["entries"][0]["sameHashAsSource"] == false);
    const auto& mod = title["installedMods"][0];
    CHECK(mod["mapping"][0]["integrity"] == "matches-record");
    CHECK(mod["inventory"]["entries"][0]["compatibility"] == "blocked");
    CHECK(mod["inventory"]["entries"][0]["loaderDependencies"].size() == 2);
    CHECK(report["firmware"]["display"] == "12.20");
    CHECK(report["akeno"]["sourceSha256"].get<std::string>().size() == 64);
    CHECK(report["activationPerformed"] == false);
    CHECK(report["gameLaunchPerformed"] == false);
    CHECK(report["hardwareVerified"] == false);
    CHECK(report["operationJournal"]["step"] == "swap");
    const auto text = report.dump();
    CHECK(text.find("copyrighted-game-bytes-not-to-be-exported") == std::string::npos);
    CHECK(text.find("PC replacement") == std::string::npos);
    CHECK(text.find("topsecret") == std::string::npos);
    CHECK(text.find("do-not-export") == std::string::npos);
    CHECK(security::readFileBounded(stateFile, 10000).value() == state);
    CHECK(security::sha256File(f.game / "asset.bin").value() == before);
    CHECK(fs::exists(f.paths.operationJournal()));
    for (const auto& r : f.http.requests()) {
        CHECK(r.method == network::HttpMethod::Post);
        CHECK((r.url.ends_with("/version") || r.url.ends_with("/settings") || r.url.ends_with("/games")));
    }
}

TEST_CASE("native inventory rejects symlinks and special files without reading or blocking") {
    test::TempDir dir;
    auto root = dir.path() / "root";
    fs::create_directory(root);
    test::writeText(dir.path() / "private", "secret");
    fs::create_symlink(dir.path() / "private", root / "link");
    REQUIRE(::mkfifo((root / "fifo").c_str(), 0600) == 0);
    auto report = diagnostics::inventory(root);
    CHECK(report["complete"] == false);
    CHECK(report["entries"].size() == 2);
    CHECK(report.dump().find("secret") == std::string::npos);
    fs::create_directory_symlink(root, dir.path() / "alias");
    CHECK(diagnostics::inventory(dir.path() / "alias")["status"] == "unavailable");
}

TEST_CASE("native budgets preserve metadata and never claim missing hashes complete") {
    test::TempDir dir;
    test::writeText(dir.path() / "a", "abc");
    test::writeText(dir.path() / "b", "def");
    diagnostics::Limits limits;
    limits.hashBytes = 2;
    auto report = diagnostics::inventory(dir.path(), limits);
    CHECK(report["complete"] == true);
    CHECK(report["hashesComplete"] == false);
    CHECK(report["entries"][0]["sha256"].is_null());
    limits.entries = 1;
    report = diagnostics::inventory(dir.path(), limits);
    CHECK(report["complete"] == false);
    CHECK(report["entries"].size() <= 1);
    CancellationToken cancel; cancel.cancel();
    CHECK(diagnostics::inventory(dir.path(), {}, &cancel)["complete"] == false);
}

TEST_CASE("native export unavailable SMP and cancelled work still save explicitly partial evidence") {
    Fixture f;
    f.http.fail(network::HttpMethod::Post, "http://127.0.0.1:10101/api/v1/version", makeError(ErrorCode::Unavailable, "offline"));
    auto report = f.run();
    CHECK(report["titles"][0]["selection"]["status"] == "unknown");
    CHECK(report["titles"][0]["vanilla"]["status"] == "unavailable");
    CancellationToken cancel; cancel.cancel();
    report = f.run(&cancel);
    CHECK(report["cancelled"] == true);
    CHECK(report["titles"].empty());
}

TEST_CASE("native export never inventories an image runtime as a vanilla game") {
    Fixture f;
    f.respond("games", Json{{"status", 0}, {"games", Json::array({{{"title_id", "PPSA24701"},
        {"path", (f.scanRoot / "game.ffpkg").string()}, {"runtime_path", f.game.string()},
        {"source_type", "image"}, {"installed_pkg", false}}})}}.dump());
    CHECK(f.run()["titles"][0]["vanilla"]["status"] == "unavailable");
}

TEST_CASE("native state path traversal is reported without reading outside the store") {
    Fixture f;
    fs::create_directory(f.paths.mods() / "PPSA24701");
    test::writeText(f.paths.mods() / "PPSA24701/state.json", R"({"schemaVersion":1,"titleId":"PPSA24701","mods":[{"downloadId":"../../outside"}]})");
    CHECK(f.run()["titles"][0]["stateError"] == "Invalid mod ID");
}

TEST_CASE("resolver models longest owning root before configured order and distinguishes other overlays") {
    Fixture f;
    auto nested = f.scanRoot / "nested";
    fs::create_directories(nested / "backports/PPSA24701");
    games::GameInfo game; game.titleId = "PPSA24701"; game.sourceType = games::SourceType::Folder;
    game.installPath = (nested / "game").string();
    shadowmount::VersionInfo version{1, "1.7beta4", {}};
    Json settings{{"scan_path_count", 2}, {"scan_paths", {f.scanRoot.string(), nested.string()}}};
    auto result = diagnostics::overlaySelection(game, version, settings, f.overlay);
    CHECK(result["status"] == "predicted-other");
    CHECK(result["inferredOwningRoot"] == nested.string());
    CHECK(result["selectedPath"] == (nested / "backports/PPSA24701").string());
    fs::remove(nested / "backports/PPSA24701");
    CHECK(diagnostics::overlaySelection(game, version, settings, f.overlay)["status"] == "predicted-akeno");
}

TEST_CASE("resolver reports unknown for inaccessible precedence, unsupported versions and malformed settings") {
    Fixture f;
    auto foreign = f.dir.path() / "foreign";
    fs::create_directory_symlink(f.scanRoot, foreign);
    games::GameInfo game; game.titleId = "PPSA24701"; game.installedPkg = true;
    shadowmount::VersionInfo version{1, "1.7beta4", {}};
    Json settings{{"scan_path_count", 2}, {"scan_paths", {foreign.string(), f.scanRoot.string()}}};
    CHECK(diagnostics::overlaySelection(game, version, settings, f.overlay)["status"] == "unknown");
    version.shadowMountVersion = "future";
    CHECK(diagnostics::overlaySelection(game, version, settings, f.overlay)["status"] == "unknown");
    version.shadowMountVersion = "1.7beta4";
    settings.erase("scan_path_count");
    CHECK(diagnostics::overlaySelection(game, version, settings, f.overlay)["status"] == "unknown");
    settings = {{"scan_paths", {"/a/../b"}}, {"scan_path_count", 1}};
    CHECK(diagnostics::overlaySelection(game, version, settings, f.overlay)["status"] == "unknown");
}

TEST_CASE("resolver checks configured roots for PKGs without inventing an owning root") {
    Fixture f;
    auto foreign = f.dir.path() / "first";
    fs::create_directories(foreign / "backports/PPSA24701");
    games::GameInfo game; game.titleId = "PPSA24701"; game.installedPkg = true;
    shadowmount::VersionInfo version{1, "1.7beta4", {}};
    Json settings{{"scan_path_count", 2}, {"scan_paths", {foreign.string(), f.scanRoot.string()}}};
    auto result = diagnostics::overlaySelection(game, version, settings, f.overlay);
    CHECK(result["status"] == "predicted-other");
    CHECK(result["inferredOwningRoot"] == "");
}

TEST_CASE("native inventory permission errors cannot be reported as a complete tree") {
    if (::geteuid() == 0) return;
    test::TempDir dir;
    auto child = dir.path() / "denied";
    fs::create_directory(child);
    fs::permissions(child, fs::perms::none);
    auto report = diagnostics::inventory(dir.path());
    fs::permissions(child, fs::perms::owner_all);
    CHECK(report["complete"] == false);
}

TEST_CASE("physical title mismatch prevents a misleading vanilla inventory") {
    Fixture f;
    test::writeText(f.game / "sce_sys/param.json", R"({"titleId":"PPSA99999","contentVersion":"01.000.000"})");
    auto report = f.run();
    CHECK(report["titles"][0]["physicalTitleMetadataMatches"] == false);
    CHECK(report["titles"][0]["vanilla"]["status"] == "unavailable");
}

TEST_CASE("disk configuration is allowlisted and cannot replace live precedence evidence") {
    Fixture f;
    f.request.smpConfigFile = f.dir.path() / "config.ini";
    test::writeText(f.request.smpConfigFile, "api_key=not-for-export\nread_only=1\nscanpath=/not-the-live-root\n");
    auto report = f.run();
    CHECK(report["shadowMountPlus"]["onDiskConfiguration"]["isRuntimeEvidence"] == false);
    CHECK(report["shadowMountPlus"]["onDiskConfiguration"]["fields"].size() == 2);
    CHECK(report["titles"][0]["selection"]["status"] == "predicted-akeno");
    CHECK(report.dump().find("not-for-export") == std::string::npos);
}

TEST_CASE("changes to settings during collection invalidate a resolver prediction") {
    Fixture f;
    auto exported = diagnostics::exportReport(f.request, f.safeFs, f.client, nullptr, [&](std::string phase) {
        if (phase.starts_with("Inventorying"))
            f.respond("settings", R"({"status":0,"scan_path_count":0,"scan_paths":[]})");
    });
    REQUIRE(exported.ok());
    auto report = readJson(*exported);
    CHECK(report["titles"][0]["selection"]["status"] == "unknown");
}

TEST_CASE("resolver distinguishes release defaults, explicit fallback and managed roots") {
    Fixture f;
    games::GameInfo game; game.titleId = "PPSA91837"; game.installedPkg = true;
    shadowmount::VersionInfo version{1, "1.7beta4", {}};
    Json settings{{"scan_path_count", 0}, {"scan_paths", Json::array()}};
    auto result = diagnostics::overlaySelection(game, version, settings, f.overlay);
    CHECK(result["usesReleaseDefaults"] == true);
    REQUIRE_FALSE(result["candidates"].empty());
    CHECK(result["candidates"][0]["path"] == "/data/homebrew/backports/PPSA91837");
    settings = {{"scan_path_count", 2}, {"scan_paths", {"/mnt/shadowmnt", f.scanRoot.string()}}};
    result = diagnostics::overlaySelection(game, version, settings, f.overlay);
    CHECK(result["usesReleaseDefaults"] == false);
    REQUIRE(result["candidates"].size() == 2);
    CHECK(result["candidates"][0]["path"] == (f.scanRoot / "backports/PPSA91837").string());
    CHECK(result["candidates"][1]["path"] == "/data/homebrew/backports/PPSA91837");
    game.installedPkg = false;
    game.installPath = f.scanRoot.string() + "-other/game";
    CHECK(diagnostics::overlaySelection(game, version, settings, f.overlay)["status"] == "unknown");
}

TEST_CASE("native export reports modified stored mappings without authorizing activation") {
    Fixture f;
    const auto store = f.paths.mods() / "PPSA24701/mod1/files";
    fs::create_directories(store);
    test::writeText(store / "asset.bin", "modified");
    Json file{{"installPath", "asset.bin"}, {"storePath", "asset.bin"}, {"size", 8}, {"sha256", std::string(64, '0')}};
    Json mod{{"downloadId", "mod1"}, {"files", Json::array({file})}};
    test::writeText(store.parent_path().parent_path() / "state.json",
        Json{{"schemaVersion", 1}, {"titleId", "PPSA24701"}, {"mods", Json::array({mod})}}.dump());
    auto report = f.run();
    CHECK(report["titles"][0]["installedMods"][0]["mapping"][0]["integrity"] == "mismatch");
    CHECK(report["activationPerformed"] == false);
}

TEST_CASE("native inventory flags case ambiguity and unsafe path names") {
    test::TempDir dir;
    test::writeText(dir.path() / "Asset.bin", "a");
    test::writeText(dir.path() / "asset.bin", "b");
    test::writeText(dir.path() / "bad:name", "c");
    auto report = diagnostics::inventory(dir.path());
    CHECK(report["complete"] == false);
    CHECK(report["findings"].size() == 2);
    diagnostics::Limits limits;
    limits.duration = std::chrono::seconds{0};
    CHECK(diagnostics::inventory(dir.path(), limits)["complete"] == false);
}
