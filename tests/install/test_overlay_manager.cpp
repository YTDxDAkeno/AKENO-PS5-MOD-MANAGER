// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <fstream>
#include <iterator>
#include <unistd.h>

#include "ArchiveTestSupport.hpp"
#include "TestSupport.hpp"
#include "akeno/install/OverlayManager.hpp"

using namespace akeno;
using namespace akeno::install;
using providers::CompatibilityStatus;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    test::TempDir dir;
    AppPaths paths;
    fs::path homebrew;
    std::unique_ptr<security::SafeFs> safeFs;
    std::unique_ptr<OperationJournal> journal;
    std::uint64_t available = 100ull * 1024 * 1024 * 1024;

    Fixture() {
        paths.root = dir.path() / "data" / "akeno-mod-manager";
        homebrew = dir.path() / "data" / "homebrew";
        fs::create_directories(homebrew);
        for (const auto& directory : paths.layout()) fs::create_directories(directory);
        safeFs = std::make_unique<security::SafeFs>(security::WriteGuard::create({paths.root}).value());
        journal = std::make_unique<OperationJournal>(paths.operationJournal(), *safeFs);
    }

    InstallEnvironment env(bool interrupted = false) {
        InstallEnvironment e{*safeFs, paths, *journal, interrupted, homebrew / "backports",
                             limits::kStorageSafetyReserveBytes, {}};
        e.storageQuery = [this](const fs::path&) -> Result<security::StorageSpace> {
            return security::StorageSpace{available * 2, available};
        };
        return e;
    }

    InstallRequest request(const std::string& id, const std::vector<test::EntrySpec>& entries,
                           CompatibilityStatus status = CompatibilityStatus::Verified) {
        InstallRequest r;
        r.downloadId = id;
        r.archive = paths.downloads() / (id + ".zip");
        REQUIRE(test::writeArchive(r.archive, mods::ArchiveFormat::Zip, entries));
        r.format = mods::ArchiveFormat::Zip;
        r.mod = {"akeno-catalogue", "example/" + id};
        r.name = "Mod " + id;
        r.version = "1.0";
        r.titleId = "PPSA90001";
        r.sourceType = games::SourceType::Folder;
        r.catalogueStatus = status;
        r.catalogueInstallable = status == CompatibilityStatus::Verified;
        return r;
    }

    fs::path backport() const { return homebrew / "backports" / "PPSA90001"; }

    std::size_t stagingEntries() const {
        return static_cast<std::size_t>(std::distance(fs::directory_iterator(paths.staging()), fs::directory_iterator{}));
    }
};

const TitleTarget kTarget{"PPSA90001", false, false, {}};

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

TEST_CASE("install, apply, later mods win, Vanilla removes only Akeno's overlay") {
    Fixture f;
    auto env = f.env();
    auto a = storeMod(f.request("aaaa1111", {{"Content/Paks/a.pak", "A", AE_IFREG, "", ""},
                                             {"Content/shared.ini", "from A", AE_IFREG, "", ""}}),
                      env);
    REQUIRE(a.ok());
    CHECK(a->files.size() == 2);
    REQUIRE(storeMod(f.request("bbbb2222", {{"Content/shared.ini", "from B", AE_IFREG, "", ""}}), env).ok());
    CHECK(f.stagingEntries() == 0);
    CHECK_FALSE(fs::exists(f.backport()));  // storing does not touch the overlay

    auto applied = applyOverlay(kTarget, env);
    REQUIRE(applied.ok());
    CHECK(applied->mods == 2);
    CHECK_FALSE(applied->vanilla);
    CHECK(readText(f.backport() / "Content" / "Paks" / "a.pak") == "A");
    CHECK(readText(f.backport() / "Content" / "shared.ini") == "from B");
    CHECK(f.stagingEntries() == 0);
    CHECK_FALSE(f.journal->load().value().has_value());
    auto state = loadTitleState(env, "PPSA90001");
    REQUIRE(state.ok());
    CHECK(state->overlayActive);

    // Turning one mod off and applying again rebuilds the overlay.
    REQUIRE(setModEnabled(env, "PPSA90001", "bbbb2222", false).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    CHECK(readText(f.backport() / "Content" / "shared.ini") == "from A");

    auto vanilla = setVanilla(kTarget, env);
    REQUIRE(vanilla.ok());
    CHECK(vanilla->vanilla);
    CHECK_FALSE(fs::exists(f.backport()));
    CHECK(fs::exists(f.homebrew / "backports"));
    CHECK_FALSE(loadTitleState(env, "PPSA90001")->overlayActive);
    // Stored mods stay and can be removed.
    REQUIRE(removeStoredMod(env, "PPSA90001", "aaaa1111").ok());
    CHECK_FALSE(fs::exists(f.paths.mods() / "PPSA90001" / "aaaa1111"));
    CHECK(loadTitleState(env, "PPSA90001")->mods.size() == 1);
}

TEST_CASE("a backport folder Akeno did not create is never touched") {
    Fixture f;
    auto env = f.env();
    fs::create_directories(f.backport() / "fakelib");
    test::writeText(f.backport() / "fakelib" / "libSceSomething.sprx", "user's firmware backport");
    REQUIRE(storeMod(f.request("aaaa1111", {{"Content/a.pak", "A", AE_IFREG, "", ""}}), env).ok());
    auto applied = applyOverlay(kTarget, env);
    REQUIRE_FALSE(applied.ok());
    CHECK(applied.error().code == ErrorCode::SafetyViolation);
    CHECK(readText(f.backport() / "fakelib" / "libSceSomething.sprx") == "user's firmware backport");
    REQUIRE_FALSE(setVanilla(kTarget, env).ok());
    CHECK(fs::exists(f.backport() / "fakelib" / "libSceSomething.sprx"));

    // An overlay replaced behind Akeno's back counts as foreign too.
    Fixture g;
    auto env2 = g.env();
    REQUIRE(storeMod(g.request("aaaa1111", {{"Content/a.pak", "A", AE_IFREG, "", ""}}), env2).ok());
    REQUIRE(applyOverlay(kTarget, env2).ok());
    fs::rename(g.backport(), g.homebrew / "moved-away");
    fs::create_directories(g.backport());
    test::writeText(g.backport() / "other.txt", "someone else");
    CHECK_FALSE(loadTitleState(env2, "PPSA90001")->overlayActive);
    CHECK_FALSE(setVanilla(kTarget, env2).ok());
    CHECK(fs::exists(g.backport() / "other.txt"));
}

TEST_CASE("nothing changes while the game is mounted or a recovery is pending") {
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("aaaa1111", {{"Content/a.pak", "A", AE_IFREG, "", ""}}), env).ok());
    auto mounted = applyOverlay(TitleTarget{"PPSA90001", true, false, {}}, env);
    REQUIRE_FALSE(mounted.ok());
    CHECK(mounted.error().code == ErrorCode::Busy);
    CHECK_FALSE(fs::exists(f.backport()));
    auto elsewhere = applyOverlay(TitleTarget{"PPSA90001", false, false, "/mnt/usb0/games/PPSA90001"}, env);
    REQUIRE_FALSE(elsewhere.ok());
    CHECK(elsewhere.error().code == ErrorCode::Unsupported);
    auto blocked = f.env(true);
    CHECK_FALSE(applyOverlay(kTarget, blocked).ok());
    CHECK_FALSE(storeMod(f.request("bbbb2222", {{"Content/b.pak", "B", AE_IFREG, "", ""}}), blocked).ok());
}

TEST_CASE("mods the analysis does not allow are not installed") {
    Fixture f;
    auto env = f.env();
    auto pc = storeMod(f.request("aaaa1111", {{"Binaries/Win64/dwmapi.dll", "MZ\x90", AE_IFREG, "", ""}}), env);
    REQUIRE_FALSE(pc.ok());
    CHECK(pc.error().message.find("cannot be installed") != std::string::npos);
    auto unknown = storeMod(
        f.request("bbbb2222", {{"Content/a.pak", "A", AE_IFREG, "", ""}}, CompatibilityStatus::Unknown), env);
    CHECK_FALSE(unknown.ok());
    CHECK(f.stagingEntries() == 0);
    CHECK_FALSE(f.journal->load().value().has_value());
    CHECK(loadTitleState(env, "PPSA90001")->mods.empty());
    CHECK_FALSE(fs::exists(f.paths.mods() / "PPSA90001" / "aaaa1111"));
}

TEST_CASE("a changed stored file stops the build and leaves the live overlay alone") {
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("aaaa1111", {{"Content/a.pak", "A", AE_IFREG, "", ""}}), env).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    test::writeText(f.paths.mods() / "PPSA90001" / "aaaa1111" / "files" / "Content" / "a.pak", "tampered");
    auto applied = applyOverlay(kTarget, env);
    REQUIRE_FALSE(applied.ok());
    CHECK(applied.error().message.find("changed") != std::string::npos);
    CHECK(readText(f.backport() / "Content" / "a.pak") == "A");
    CHECK(f.stagingEntries() == 0);
}

TEST_CASE("installed packages are limited to 256 redirects") {
    Fixture f;
    auto env = f.env();
    std::vector<test::EntrySpec> many;
    for (int i = 0; i < 300; ++i) many.push_back({"Content/f" + std::to_string(i) + ".pak", "x", AE_IFREG, "", ""});
    REQUIRE(storeMod(f.request("aaaa1111", many), env).ok());
    auto applied = applyOverlay(TitleTarget{"PPSA90001", false, true, {}}, env);
    REQUIRE_FALSE(applied.ok());
    CHECK(applied.error().message.find("256") != std::string::npos);
    CHECK_FALSE(fs::exists(f.backport()));
}

TEST_CASE("a damaged state file is refused, not guessed at") {
    Fixture f;
    auto env = f.env();
    fs::create_directories(f.paths.mods() / "PPSA90001");
    test::writeText(statePath(f.paths, "PPSA90001"),
                    R"({"schemaVersion":1,"titleId":"PPSA90001","mods":[{"downloadId":"x","files":[)"
                    R"({"installPath":"../../escape","storePath":"a","size":1,"sha256":")" +
                        std::string(64, 'a') + R"("}]}]})");
    CHECK_FALSE(loadTitleState(env, "PPSA90001").ok());
    CHECK_FALSE(loadTitleState(env, "../etc").ok());
}

TEST_CASE("a PC mod may add files but never replace the game's own") {
    Fixture f;
    auto env = f.env();
    const fs::path game = f.dir.path() / "data" / "homebrew" / "Some Game";
    fs::create_directories(game / "data");
    test::writeText(game / "data" / "chara.bin", "original");

    auto replacing = f.request("aaaa1111", {{"data/chara.bin", "pc version", AE_IFREG, "", ""},
                                            {"data/new.bin", "new", AE_IFREG, "", ""}});
    replacing.pcSource = true;
    replacing.gameFolder = game.string();
    auto refused = storeMod(replacing, env);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.find("replace 1 of the game's own files") != std::string::npos);
    CHECK(f.stagingEntries() == 0);

    auto adding = f.request("bbbb2222", {{"data/new.bin", "new", AE_IFREG, "", ""}});
    adding.pcSource = true;
    adding.gameFolder = game.string();
    CHECK(storeMod(adding, env).ok());

    auto unknownFolder = f.request("cccc3333", {{"data/other.bin", "x", AE_IFREG, "", ""}});
    unknownFolder.pcSource = true;
    CHECK_FALSE(storeMod(unknownFolder, env).ok());
}

TEST_CASE("PC checks fail closed for missing roots, parent files and links") {
    Fixture f;
    auto env = f.env();
    auto request = f.request("pc-check", {{"data/new.bin", "PC data", AE_IFREG, "", ""}});
    request.pcSource = true;
    request.gameFolder = (f.homebrew / "missing").string();
    CHECK_FALSE(storeMod(request, env).ok());
    fs::create_directories(request.gameFolder);
    test::writeText(fs::path(request.gameFolder) / "data", "a file, not a directory");
    CHECK_FALSE(storeMod(request, env).ok());
    fs::remove(fs::path(request.gameFolder) / "data");
    fs::create_directory_symlink(f.homebrew, fs::path(request.gameFolder) / "data");
    CHECK_FALSE(storeMod(request, env).ok());
    CHECK(loadTitleState(env, request.titleId)->mods.empty());
}

TEST_CASE("activation rechecks PC provenance after a game update adds a target") {
    Fixture f;
    auto env = f.env();
    const auto game = f.homebrew / "game";
    fs::create_directories(game);
    auto request = f.request("pc-addition", {{"new.bin", "PC data", AE_IFREG, "", ""}});
    request.pcSource = true;
    request.gameFolder = game.string();
    REQUIRE(storeMod(request, env).ok());
    REQUIRE(loadTitleState(env, request.titleId)->mods[0].pcSource);
    const TitleTarget target{request.titleId, false, false, game.string()};
    REQUIRE(applyOverlay(target, env).ok());
    test::writeText(game / "new.bin", "original update data");
    CHECK_FALSE(applyOverlay(target, env).ok());
    CHECK(readText(game / "new.bin") == "original update data");
    CHECK(readText(f.backport() / "new.bin") == "PC data");
}

TEST_CASE("previously installed loader markers cannot bypass current activation checks") {
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("old-mod", {{"payload.bin", "data", AE_IFREG, "", ""}}), env).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    // Simulate a valid old state that predates loader detection, retaining matching hashes.
    const auto state = statePath(f.paths, kTarget.titleId);
    std::string document = readText(state);
    std::size_t pos = 0;
    while ((pos = document.find("payload.bin", pos)) != std::string::npos) {
        document.replace(pos, 11, "ModConfig.json");
        pos += 14;
    }
    test::writeText(state, document);
    fs::rename(f.paths.mods() / kTarget.titleId / "old-mod/files/payload.bin",
               f.paths.mods() / kTarget.titleId / "old-mod/files/ModConfig.json");
    auto applied = applyOverlay(kTarget, env);
    REQUIRE_FALSE(applied.ok());
    CHECK(applied.error().message.find("Reloaded-II") != std::string::npos);
    CHECK(readText(f.backport() / "payload.bin") == "data");
    REQUIRE(setVanilla(kTarget, env).ok());  // unsafe stored mods can still be deactivated
    CHECK_FALSE(fs::exists(f.backport()));
}

TEST_CASE("ambiguous case across mods is refused before swapping the live overlay") {
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("a", {{"Data/a.bin", "A", AE_IFREG, "", ""}}), env).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    REQUIRE(storeMod(f.request("b", {{"data/b.bin", "B", AE_IFREG, "", ""}}), env).ok());
    CHECK_FALSE(applyOverlay(kTarget, env).ok());
    CHECK(readText(f.backport() / "Data/a.bin") == "A");
    CHECK_FALSE(fs::exists(f.backport() / "data/b.bin"));
}

TEST_CASE("failed Vanilla request preserves mod selections and owned overlay") {
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("a", {{"a.bin", "A", AE_IFREG, "", ""}}), env).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    CHECK_FALSE(setVanilla({kTarget.titleId, true, false, {}}, env).ok());
    CHECK(loadTitleState(env, kTarget.titleId)->enabledCount() == 1);
    CHECK_FALSE(setVanilla({kTarget.titleId, false, false, "/outside/game"}, env).ok());
    CHECK(loadTitleState(env, kTarget.titleId)->enabledCount() == 1);
    CHECK(loadTitleState(env, kTarget.titleId)->overlayActive);
    CHECK(readText(f.backport() / "a.bin") == "A");
}

TEST_CASE("514 files are not rejected by the installed-package limit for folder games") {
    Fixture f;
    auto env = f.env();
    std::vector<test::EntrySpec> files;
    for (int i = 0; i < 514; ++i) files.push_back({"data/f" + std::to_string(i), "x", AE_IFREG, "", ""});
    REQUIRE(storeMod(f.request("many", files), env).ok());
    auto applied = applyOverlay(kTarget, env);
    REQUIRE(applied.ok());
    CHECK(applied->files == 514);
}

TEST_CASE("journal write failure prevents an overlay swap") {
    if (::geteuid() == 0) { MESSAGE("permission fault test requires an unprivileged runner"); return; }
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("a", {{"a.bin", "A", AE_IFREG, "", ""}}), env).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    REQUIRE(storeMod(f.request("b", {{"b.bin", "B", AE_IFREG, "", ""}}), env).ok());
    const auto originalPermissions = fs::status(f.paths.root).permissions();
    env.storageQuery = [&](const fs::path&) -> Result<security::StorageSpace> {
        fs::permissions(f.paths.root, fs::perms::owner_read | fs::perms::owner_exec);
        return security::StorageSpace{f.available * 2, f.available};
    };
    auto applied = applyOverlay(kTarget, env);
    fs::permissions(f.paths.root, originalPermissions);
    CHECK_FALSE(applied.ok());
    CHECK(readText(f.backport() / "a.bin") == "A");
    CHECK_FALSE(fs::exists(f.backport() / "b.bin"));
    CHECK(loadTitleState(env, kTarget.titleId)->overlayActive);
}

TEST_CASE("state save failure restores the previous overlay and retains recovery evidence") {
    if (::geteuid() == 0) { MESSAGE("permission fault test requires an unprivileged runner"); return; }
    Fixture f;
    auto env = f.env();
    REQUIRE(storeMod(f.request("a", {{"a.bin", "A", AE_IFREG, "", ""}}), env).ok());
    REQUIRE(applyOverlay(kTarget, env).ok());
    REQUIRE(storeMod(f.request("b", {{"b.bin", "B", AE_IFREG, "", ""}}), env).ok());
    const auto stateDirectory = statePath(f.paths, kTarget.titleId).parent_path();
    const auto originalPermissions = fs::status(stateDirectory).permissions();
    env.storageQuery = [&](const fs::path&) -> Result<security::StorageSpace> {
        fs::permissions(stateDirectory, fs::perms::owner_read | fs::perms::owner_exec);
        return security::StorageSpace{f.available * 2, f.available};
    };
    auto applied = applyOverlay(kTarget, env);
    fs::permissions(stateDirectory, originalPermissions);
    CHECK_FALSE(applied.ok());
    CHECK(readText(f.backport() / "a.bin") == "A");
    CHECK_FALSE(fs::exists(f.backport() / "b.bin"));
    CHECK(loadTitleState(env, kTarget.titleId)->overlayActive);
    REQUIRE(f.journal->load().value().has_value());
    CHECK(f.journal->load().value()->activeOverlayTouched);
}
