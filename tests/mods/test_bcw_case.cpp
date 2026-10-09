// SPDX-License-Identifier: GPL-3.0-or-later
// The Better Carry Weight x10 case (PPSA28000), end to end on the host: the archive structure of
// tests/fixtures/mods/better-carry-weight-x10/archive.json with synthetic containers, against a
// synthetic game folder laid out like the observed PS5 installation.
#include "Doctest.hpp"

#include <map>

#include "ArchiveTestSupport.hpp"
#include "TestSupport.hpp"
#include "UnrealTestSupport.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/install/OverlayManager.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/security/Sha256.hpp"

using namespace akeno;
using json::Json;
namespace fs = std::filesystem;

namespace {

const std::string kPackage = "/Game/_Dawnwalker/Player/BP_PlayerCharacter";
const std::vector<std::string> kImports{"/Game/_Dawnwalker/Player/MPC_Player",
                                        "/Game/_Dawnwalker/Player/Effects/GE_Encumbered",
                                        "/Game/_Dawnwalker/Player/DA_PlayerAbilityConfig"};

struct GameOptions {
    int tocVersion = 8;
    bool signedContainers = false;
    bool withImports = true;
};

struct Fixture {
    test::TempDir dir;
    AppPaths paths;
    fs::path homebrew;
    fs::path game;
    std::unique_ptr<security::SafeFs> safeFs;
    std::unique_ptr<OperationJournal> journal;
    Json facts;

    explicit Fixture(GameOptions options = {}) {
        paths.root = dir.path() / "data" / "akeno-mod-manager";
        homebrew = dir.path() / "data" / "homebrew";
        game = homebrew / "PPSA28000";
        for (const auto& directory : paths.layout()) fs::create_directories(directory);
        safeFs = std::make_unique<security::SafeFs>(security::WriteGuard::create({paths.root}).value());
        journal = std::make_unique<OperationJournal>(paths.operationJournal(), *safeFs);
        facts = Json::parse(test::readFixture("mods/better-carry-weight-x10/archive.json"));

        // The game's own files, as listed by the PPSA28000 diagnostic (contents are synthetic).
        fs::create_directories(game / "sce_sys");
        fs::create_directories(game / "dawnwalker/content/paks");
        fs::create_directories(game / "dawnwalker/content/movies");
        test::writeText(game / "sce_sys/param.json", R"({"titleId":"PPSA28000","contentVersion":"01.005.000"})");
        test::writeText(game / "eboot.bin", "\x7F" "ELF synthetic");
        test::writeText(game / "dawnwalker/content/movies/intro_cgi.bk2", "movie");
        std::vector<test::ChunkSpec> chunks{{unreal::packageIdFromName(kPackage), 1, "original package"}};
        if (options.withImports) {
            for (const auto& name : kImports) chunks.push_back({unreal::packageIdFromName(name), 1, "imported " + name});
        }
        chunks.push_back({0x5050505050505050ull, 6, test::containerHeader(0x5050505050505050ull, {})});
        auto containers = test::ioStore(0x5050505050505050ull, chunks, "dawnwalker.uasset", options.tocVersion);
        if (options.signedContainers) containers.utoc[80] = static_cast<char>(0x0C);
        test::writeText(game / "dawnwalker/content/paks/dawnwalker-ps5.utoc", containers.utoc);
        test::writeText(game / "dawnwalker/content/paks/dawnwalker-ps5.ucas", containers.ucas);
        test::writeText(game / "dawnwalker/content/paks/dawnwalker-ps5.pak", test::pakStub());
    }

    // The archive: the same entries as the real one, with synthetic container bytes.
    fs::path archive() {
        const auto set = test::packageSet(kPackage, kImports);
        std::vector<test::EntrySpec> entries;
        for (const auto& entry : facts["archive"]["entries"]) {
            const std::string path = entry["path"];
            if (entry.value("type", "") == "directory") {
                entries.push_back({path, "", AE_IFDIR, "", ""});
            } else if (strings::endsWith(path, ".pak")) {
                entries.push_back({path, set.pak, AE_IFREG, "", ""});
            } else if (strings::endsWith(path, ".utoc")) {
                entries.push_back({path, set.utoc, AE_IFREG, "", ""});
            } else {
                entries.push_back({path, set.ucas, AE_IFREG, "", ""});
            }
        }
        const auto file = paths.downloads() / "dac30f1aa5840955.zip";
        REQUIRE(test::writeArchive(file, mods::ArchiveFormat::Zip, entries));
        return file;
    }

    std::map<std::string, std::string> gameHashes() const {
        std::map<std::string, std::string> hashes;
        for (const auto& entry : fs::recursive_directory_iterator(game)) {
            hashes[fs::relative(entry.path(), game).string()] =
                entry.is_regular_file() ? security::sha256File(entry.path()).value() : std::string("dir");
        }
        return hashes;
    }

    mods::ModCheckReport check() {
        mods::ModCheckRequest request;
        request.downloadId = "dac30f1aa5840955";
        request.archive = archive();
        request.format = mods::ArchiveFormat::Zip;
        request.mod = {"nexus", "1234"};
        request.displayName = "Better Carry Weight x10";
        request.modVersion = "1.0";
        request.titleId = "PPSA28000";
        request.sourceType = games::SourceType::Folder;
        request.catalogueStatus = providers::CompatibilityStatus::Experimental;
        request.catalogueInstallable = true;
        request.pcSource = true;
        request.gameVersion = "01.005.000";
        request.contentId = "UP0000-PPSA28000_00-0000000000000000";
        request.gameFolder = game.string();
        mods::ModCheckEnvironment env{*safeFs, paths, *journal, false, limits::kStorageSafetyReserveBytes, {},
                                      [](const fs::path&) -> Result<security::StorageSpace> {
                                          return security::StorageSpace{1ull << 40, 1ull << 39};
                                      }};
        auto report = mods::runModCheck(request, env);
        REQUIRE(report.ok());
        mods::completeReport(report.value(), paths, false);
        return std::move(report).value();
    }

    install::InstallEnvironment environment(const compatibility::Registry* registry = nullptr) {
        install::InstallEnvironment env{*safeFs, paths, *journal, false, homebrew / "backports",
                                        limits::kStorageSafetyReserveBytes, {}, registry};
        env.storageQuery = [](const fs::path&) -> Result<security::StorageSpace> {
            return security::StorageSpace{1ull << 40, 1ull << 39};
        };
        return env;
    }

    install::InstallRequest installRequest(const std::string& gameVersion = "01.005.000") {
        install::InstallRequest request;
        request.downloadId = "dac30f1aa5840955";
        request.archive = archive();
        request.format = mods::ArchiveFormat::Zip;
        request.mod = {"nexus", "1234"};
        request.name = "Better Carry Weight x10";
        request.version = "1.0";
        request.titleId = "PPSA28000";
        request.sourceType = games::SourceType::Folder;
        request.catalogueStatus = providers::CompatibilityStatus::Experimental;
        request.catalogueInstallable = true;
        request.pcSource = true;
        request.gameFolder = game.string();
        request.gameVersion = gameVersion;
        request.archiveSha256 = facts["archive"]["sha256"];
        return request;
    }
};

// What a hardware-tested rule for this game would look like. It is test data only: no such
// evidence exists, so no adapter for PPSA28000 ships.
class HypotheticalDawnwalkerRule final : public compatibility::IGameAdapter {
public:
    std::string id() const override { return "hypothetical-ppsa28000"; }
    bool appliesTo(std::string_view titleId) const override { return titleId == "PPSA28000"; }
    std::vector<compatibility::LoadingConvention> conventions() const override {
        return {{"dawnwalker/content/paks/~mods", {"pak", "utoc", "ucas"}, {"01.005.000"}, "test data, not evidence", true}};
    }
};

}  // namespace

TEST_CASE("Better Carry Weight x10: wrapper detected, package set verified, ~mods only a candidate") {
    Fixture f;
    const auto report = f.check();

    // Archive layout.
    CHECK(report.layout.rule == "unreal-flat");
    CHECK(report.layout.packagingFolders == std::vector<std::string>{"Better Carry Weight x10"});
    CHECK(report.layout.targetPrefix == "dawnwalker/content/paks/~mods");
    CHECK(report.layout.confidence == mods::MappingConfidence::Candidate);
    std::map<std::string, std::string> mapping;
    for (const auto& file : report.analysis.files) mapping[file.archivePath] = file.installPath;
    CHECK(mapping["Better Carry Weight x10/00000000_BetterCarryWeightx10_P.utoc"] ==
          "dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.utoc");
    for (const auto& file : report.analysis.files) CHECK(file.target == mods::TargetState::New);

    // The package set.
    REQUIRE(report.unreal.sets.size() == 1);
    const auto& set = report.unreal.sets[0];
    CHECK(set.name == "Better Carry Weight x10/00000000_BetterCarryWeightx10_P");
    CHECK(set.companionsVerified);
    CHECK(set.tocVersion == 8);
    CHECK(set.pakVersion == 11);
    CHECK(set.containerId == "e4a7420854dad984");
    CHECK(set.packages == std::vector<std::string>{kPackage});
    CHECK(report.unreal.unversioned);
    CHECK(report.unreal.importedTotal == kImports.size());

    // The game.
    CHECK(report.game.listed);
    CHECK(report.game.complete);
    CHECK(report.game.paksDirectory == "dawnwalker/content/paks");
    CHECK(report.game.maxTocVersion == 8);
    CHECK(report.game.region == "Americas (UP)");

    // The decision: four separate answers, activation refused.
    const auto& a = report.assessment;
    CHECK(a.outcome == compatibility::Outcome::Unknown);
    CHECK(a.category == compatibility::ModCategory::PotentiallyPortable);
    CHECK(a.mappingConfidence == mods::MappingConfidence::Candidate);
    CHECK(a.loading == compatibility::LoadingSupport::Unverified);
    CHECK(a.platform == compatibility::PlatformCompatibility::Unknown);
    CHECK_FALSE(a.activationAllowed);
    CHECK(a.engine == "Unreal Engine");
    CHECK(a.pcDependencies.empty());
    auto evidence = [&](const std::string& text) {
        return std::any_of(a.evidence.begin(), a.evidence.end(), [&](const auto& e) { return e.text.find(text) != std::string::npos; });
    };
    CHECK(evidence("same IoStore version as the game's own containers (8)"));
    CHECK(evidence("1 of 1 package(s) in the mod"));
    CHECK(evidence("All 3 game packages the mod imports exist in the installed PS5 game"));
    CHECK(evidence("unversioned properties"));
    CHECK(evidence("belong together"));
    REQUIRE_FALSE(a.blockedReasons.empty());
    CHECK(a.blockedReasons[0].find("only a candidate") != std::string::npos);
    CHECK(std::any_of(a.risks.begin(), a.risks.end(), [](const std::string& r) { return r.find("BP_PlayerCharacter") != std::string::npos; }));
    REQUIRE(report.plan.has_value());
    CHECK_FALSE(report.plan->executable);
    CHECK(report.plan->additions == 3);
    CHECK(report.plan->mapping[0].second ==
          "/data/homebrew/backports/PPSA28000/dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.pak");

    // Stored and read back with every answer intact.
    REQUIRE(mods::saveReport(*f.safeFs, f.paths, report).ok());
    auto loaded = mods::loadReport(f.paths, "dac30f1aa5840955");
    REQUIRE(loaded.ok());
    CHECK(loaded->assessment.outcome == compatibility::Outcome::Unknown);
    CHECK(loaded->assessment.mappingConfidence == mods::MappingConfidence::Candidate);
    CHECK_FALSE(loaded->assessment.activationAllowed);
    CHECK(loaded->layout.packagingFolders == report.layout.packagingFolders);
    CHECK(loaded->unreal.sets.size() == 1);
    CHECK(loaded->game.paksDirectory == "dawnwalker/content/paks");
}

TEST_CASE("Better Carry Weight x10: the installer refuses the unverified mapping and leaves the game untouched") {
    Fixture f;
    const auto before = f.gameHashes();
    auto env = f.environment();
    auto stored = install::storeMod(f.installRequest(), env);
    REQUIRE_FALSE(stored.ok());
    CHECK(stored.error().message.find("only a candidate") != std::string::npos);
    CHECK(install::loadTitleState(env, "PPSA28000")->mods.empty());
    CHECK_FALSE(fs::exists(f.homebrew / "backports" / "PPSA28000"));
    CHECK(fs::is_empty(f.paths.staging()));
    (void)f.check();
    CHECK(f.gameHashes() == before);
}

TEST_CASE("Better Carry Weight x10: with hardware evidence for this game version the mapped layout would be installed") {
    Fixture f;
    const auto before = f.gameHashes();
    compatibility::Registry registry;
    registry.addAdapter(std::make_shared<HypotheticalDawnwalkerRule>());
    auto env = f.environment(&registry);
    // Evidence for another game version does not count.
    CHECK_FALSE(install::storeMod(f.installRequest("01.006.000"), env).ok());
    auto stored = install::storeMod(f.installRequest(), env);
    REQUIRE(stored.ok());
    CHECK(stored->mappingConfidence == "established");
    CHECK(stored->outcome == "LIKELY_COMPATIBLE");
    CHECK(stored->archiveRoot == "Better Carry Weight x10");
    REQUIRE(install::applyOverlay({"PPSA28000", false, false, f.game.string()}, env).ok());
    const auto backport = f.homebrew / "backports" / "PPSA28000";
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.pak"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.utoc"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.ucas"));
    CHECK_FALSE(fs::exists(backport / "Better Carry Weight x10"));
    CHECK(f.gameHashes() == before);  // the original game is never written
    REQUIRE(install::setVanilla({"PPSA28000", false, false, f.game.string()}, env).ok());
    CHECK_FALSE(fs::exists(backport));
    CHECK(f.gameHashes() == before);
}

TEST_CASE("Better Carry Weight x10: a game with an older IoStore version, missing imports or signed containers") {
    {
        Fixture f({7, false, true});
        const auto report = f.check();
        CHECK(report.assessment.outcome == compatibility::Outcome::Incompatible);
        CHECK(report.assessment.platform == compatibility::PlatformCompatibility::Incompatible);
        CHECK(std::any_of(report.assessment.evidence.begin(), report.assessment.evidence.end(), [](const auto& e) {
            return e.text.find("newer than the game's (7)") != std::string::npos;
        }));
    }
    {
        Fixture f({8, false, false});
        const auto report = f.check();
        CHECK(report.assessment.outcome == compatibility::Outcome::Incompatible);
        CHECK(std::any_of(report.assessment.evidence.begin(), report.assessment.evidence.end(), [](const auto& e) {
            return e.text.find("3 of 3 game packages the mod imports do not exist") != std::string::npos;
        }));
    }
    {
        Fixture f({8, true, true});
        const auto report = f.check();
        CHECK(report.game.signedContainers);
        CHECK(std::any_of(report.assessment.risks.begin(), report.assessment.risks.end(), [](const std::string& r) {
            return r.find("signed") != std::string::npos;
        }));
        CHECK_FALSE(report.assessment.activationAllowed);
    }
}
