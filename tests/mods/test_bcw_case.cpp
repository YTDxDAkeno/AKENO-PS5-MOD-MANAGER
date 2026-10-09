// SPDX-License-Identifier: GPL-3.0-or-later
// The Better Carry Weight x10 case (PPSA28000), end to end on the host: the archive structure of
// tests/fixtures/mods/better-carry-weight-x10/archive.json with synthetic containers, against a
// synthetic game folder laid out like the observed PS5 installation.
#include "Doctest.hpp"

#include <algorithm>
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
    bool bulkData = false;  // the mod's container also carries bulk data (textures, meshes, audio)

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
        const auto set = test::packageSet(kPackage, kImports, 0xe4a7420854dad984ull, bulkData);
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

    mods::ModCheckReport check(const compatibility::Registry* registry = nullptr, std::string localEvidence = {},
                               std::string modVersion = "1.0") {
        mods::ModCheckRequest request;
        request.downloadId = "dac30f1aa5840955";
        request.archive = archive();
        request.format = mods::ArchiveFormat::Zip;
        request.mod = {"nexus", "1234"};
        request.displayName = "Better Carry Weight x10";
        request.modVersion = std::move(modVersion);
        request.registry = registry;
        request.localEvidence = std::move(localEvidence);
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
          "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.utoc");
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
          "/data/homebrew/backports/PPSA28000/dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.pak");

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
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.pak"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.utoc"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.ucas"));
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

namespace {

bool mentions(const std::vector<std::string>& items, const std::string& text) {
    return std::any_of(items.begin(), items.end(), [&](const std::string& item) { return item.find(text) != std::string::npos; });
}

const install::TitleTarget kDawnwalker(const Fixture& f) { return {"PPSA28000", false, false, f.game.string()}; }

}  // namespace

TEST_CASE("Better Carry Weight x10: a test install is offered with its risks, and the offer is stored") {
    Fixture f;
    const auto report = f.check();
    const auto& a = report.assessment;
    CHECK_FALSE(a.activationAllowed);
    CHECK(a.testInstallAvailable);
    CHECK(a.testBlockers.empty());
    CHECK(mentions(a.testRisks, "loads extra package files from dawnwalker/content/paks/~mods"));
    CHECK(mentions(a.testRisks, "BP_PlayerCharacter"));
    CHECK(mentions(a.testRisks, "Back up your saved data"));
    CHECK(mentions(a.testRisks, "turn the mod off in Installed Mods"));
    REQUIRE(mods::saveReport(*f.safeFs, f.paths, report).ok());
    auto loaded = mods::loadReport(f.paths, "dac30f1aa5840955");
    REQUIRE(loaded.ok());
    CHECK(loaded->assessment.testInstallAvailable);
    CHECK(loaded->assessment.testRisks == a.testRisks);
}

TEST_CASE("Better Carry Weight x10: test install, a 'works' report, and what later checks make of it") {
    Fixture f;
    const auto before = f.gameHashes();
    auto env = f.environment();
    auto request = f.installRequest();
    request.test = true;
    auto stored = install::storeMod(request, env);
    REQUIRE(stored.ok());
    CHECK(stored->test);
    CHECK(stored->activationAllowed);
    CHECK(stored->outcome == "EXPERIMENTAL");
    CHECK(stored->assessedOutcome == "UNKNOWN");
    CHECK(stored->gameVersion == "01.005.000");
    CHECK(stored->testPlacement == install::TestPlacement::ModsFolder);
    auto applied = install::applyOverlay(kDawnwalker(f), env);
    REQUIRE(applied.ok());
    CHECK(applied->turnedOff.empty());
    const auto backport = f.homebrew / "backports" / "PPSA28000";
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.pak"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.utoc"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.ucas"));
    CHECK(f.gameHashes() == before);

    auto state = install::loadTitleState(env, "PPSA28000");
    REQUIRE(state.ok());
    REQUIRE(state->mods.size() == 1);
    CHECK(state->mods[0].test);
    CHECK(state->mods[0].testResult == install::TestResult::Untested);
    auto records = state->testRecords();
    REQUIRE(records.size() == 1);
    CHECK(records[0].directory == "dawnwalker/content/paks/~mods");
    CHECK(install::evidenceStamp(records).empty());  // nothing reported yet

    auto turnedOff = install::reportTestResult(env, "PPSA28000", "dac30f1aa5840955", install::TestResult::Works);
    REQUIRE(turnedOff.ok());
    CHECK_FALSE(turnedOff.value());
    records = install::loadTitleState(env, "PPSA28000")->testRecords();
    REQUIRE(records[0].result == install::TestResult::Works);
    CHECK_FALSE(records[0].reportedAt.empty());
    const std::string stamp = install::evidenceStamp(records);
    CHECK(security::isSha256Hex(stamp));

    // The same mod version on the same game version: the user's report is the record it needed.
    const auto registry = install::registryWithReports("PPSA28000", records);
    const auto again = f.check(&registry, stamp);
    CHECK(again.localEvidence == stamp);
    CHECK(again.assessment.loading == compatibility::LoadingSupport::Verified);
    CHECK(again.assessment.mappingConfidence == mods::MappingConfidence::Established);
    CHECK(again.assessment.outcome == compatibility::Outcome::VerifiedPs5);
    CHECK(again.assessment.activationAllowed);
    CHECK_FALSE(again.assessment.testInstallAvailable);
    CHECK(std::any_of(again.assessment.evidence.begin(), again.assessment.evidence.end(), [](const auto& e) {
        return e.source == compatibility::EvidenceSource::Report && e.text.find("you reported") != std::string::npos;
    }));
    // Another version of the mod: the folder is known to load, the content is not.
    const auto other = f.check(&registry, stamp, "1.1");
    CHECK(other.assessment.loading == compatibility::LoadingSupport::Verified);
    CHECK_FALSE(other.assessment.activationAllowed);
    CHECK(other.assessment.testInstallAvailable);
    CHECK(f.gameHashes() == before);
}

TEST_CASE("Better Carry Weight x10: a crash report turns the test off and rules out another test") {
    Fixture f;
    auto env = f.environment();
    auto request = f.installRequest();
    request.test = true;
    REQUIRE(install::storeMod(request, env).ok());
    REQUIRE(install::applyOverlay(kDawnwalker(f), env).ok());
    auto turnedOff = install::reportTestResult(env, "PPSA28000", "dac30f1aa5840955", install::TestResult::Crashed);
    REQUIRE(turnedOff.ok());
    CHECK(turnedOff.value());
    auto applied = install::applyOverlay(kDawnwalker(f), env);
    REQUIRE(applied.ok());
    CHECK(applied->vanilla);
    CHECK_FALSE(fs::exists(f.homebrew / "backports" / "PPSA28000"));
    // Turning it on again does not bring it back.
    REQUIRE(install::setModEnabled(env, "PPSA28000", "dac30f1aa5840955", true).ok());
    applied = install::applyOverlay(kDawnwalker(f), env);
    REQUIRE(applied.ok());
    REQUIRE(applied->turnedOff.size() == 1);
    CHECK(applied->turnedOff[0].find("crashed the game") != std::string::npos);
    CHECK_FALSE(fs::exists(f.homebrew / "backports" / "PPSA28000"));

    const auto records = install::loadTitleState(env, "PPSA28000")->testRecords();
    const auto registry = install::registryWithReports("PPSA28000", records);
    const auto report = f.check(&registry, install::evidenceStamp(records));
    CHECK_FALSE(report.assessment.testInstallAvailable);
    CHECK(mentions(report.assessment.testBlockers, "crashed this game"));
    env.registry = &registry;
    request.downloadId = "dac30f1aa5840956";
    auto again = install::storeMod(request, env);
    REQUIRE_FALSE(again.ok());
    CHECK(again.error().message.find("cannot be installed as a test") != std::string::npos);
    // Only test installs take a result.
    CHECK_FALSE(install::reportTestResult(env, "PPSA28000", "missing", install::TestResult::Works).ok());
}

TEST_CASE("Better Carry Weight x10: the package-folder placement puts the files next to the game's containers") {
    Fixture f;
    const auto before = f.gameHashes();
    auto env = f.environment();
    auto request = f.installRequest();
    request.test = true;
    request.placement = install::TestPlacement::PaksFolder;
    auto stored = install::storeMod(request, env);
    REQUIRE(stored.ok());
    CHECK(stored->testPlacement == install::TestPlacement::PaksFolder);
    CHECK(stored->targetPrefix == "dawnwalker/content/paks");
    REQUIRE(install::applyOverlay(kDawnwalker(f), env).ok());
    const auto backport = f.homebrew / "backports" / "PPSA28000";
    CHECK(fs::exists(backport / "dawnwalker/content/paks/00000000_bettercarryweightx10_p.pak"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/00000000_bettercarryweightx10_p.utoc"));
    CHECK(fs::exists(backport / "dawnwalker/content/paks/00000000_bettercarryweightx10_p.ucas"));
    CHECK_FALSE(fs::exists(backport / "dawnwalker/content/paks/~mods"));
    CHECK(f.gameHashes() == before);
    const auto records = install::loadTitleState(env, "PPSA28000")->testRecords();
    REQUIRE(records.size() == 1);
    CHECK(records[0].directory == "dawnwalker/content/paks");
    CHECK(records[0].placement == install::TestPlacement::PaksFolder);
    REQUIRE(install::setVanilla(kDawnwalker(f), env).ok());
    CHECK(f.gameHashes() == before);
}

TEST_CASE("Better Carry Weight x10: no test install for signed games or containers with bulk data") {
    {
        Fixture f({8, true, true});
        const auto report = f.check();
        CHECK_FALSE(report.assessment.testInstallAvailable);
        CHECK(mentions(report.assessment.testBlockers, "signed"));
        auto env = f.environment();
        auto request = f.installRequest();
        request.test = true;
        auto stored = install::storeMod(request, env);
        REQUIRE_FALSE(stored.ok());
        CHECK(stored.error().message.find("signed") != std::string::npos);
    }
    {
        Fixture f;
        f.bulkData = true;
        const auto report = f.check();
        CHECK_FALSE(report.assessment.testInstallAvailable);
        CHECK(mentions(report.assessment.testBlockers, "BulkData"));
    }
    {
        Fixture f({7, false, true});  // older game engine: incompatible, so no test either
        const auto report = f.check();
        CHECK_FALSE(report.assessment.testInstallAvailable);
        CHECK(mentions(report.assessment.testBlockers, "incompatible"));
    }
}

TEST_CASE("Better Carry Weight x10: a test install replaces the copy 0.2.0-alpha stored for the same download") {
    Fixture f;
    // The state 0.2.0-alpha left on the console: the wrapper folder copied as is, no decision.
    const std::string hash(64, 'a');
    Json legacy = {{"schemaVersion", 1},
                   {"titleId", "PPSA28000"},
                   {"overlayActive", false},
                   {"backportPath", ""},
                   {"backportDevice", ""},
                   {"backportInode", ""},
                   {"appliedAt", ""},
                   {"mods", Json::array({{{"downloadId", "dac30f1aa5840955"},
                                          {"provider", "nexus"},
                                          {"modId", "1234"},
                                          {"name", "Better Carry Weight x10"},
                                          {"version", "1.0"},
                                          {"enabled", true},
                                          {"pcSource", true},
                                          {"bytes", 347},
                                          {"installedAt", "2026-10-08T16:40:00Z"},
                                          {"files", Json::array({{{"installPath", "Better Carry Weight x10/00000000_BetterCarryWeightx10_P.pak"},
                                                                  {"storePath", "Better Carry Weight x10/00000000_BetterCarryWeightx10_P.pak"},
                                                                  {"size", 347},
                                                                  {"sha256", hash}}})}}})}};
    fs::create_directories(f.paths.mods() / "PPSA28000" / "dac30f1aa5840955" / "files");
    test::writeText(install::statePath(f.paths, "PPSA28000"), legacy.dump(2));
    auto env = f.environment();
    auto request = f.installRequest();
    request.test = true;
    auto stored = install::storeMod(request, env);
    REQUIRE(stored.ok());
    auto state = install::loadTitleState(env, "PPSA28000");
    REQUIRE(state.ok());
    REQUIRE(state->mods.size() == 1);
    CHECK(state->mods[0].test);
    CHECK(state->mods[0].activationRecorded);
    CHECK(state->mods[0].files.size() == 3);
    CHECK(state->mods[0].files[0].installPath.rfind("dawnwalker/content/paks/~mods/", 0) == 0);
    auto applied = install::applyOverlay(kDawnwalker(f), env);
    REQUIRE(applied.ok());
    CHECK(applied->turnedOff.empty());
    CHECK_FALSE(fs::exists(f.homebrew / "backports" / "PPSA28000" / "Better Carry Weight x10"));
    // A checked install of the same download is not replaced again.
    auto again = install::storeMod(request, env);
    REQUIRE_FALSE(again.ok());
    CHECK(again.error().code == ErrorCode::AlreadyExists);
}

TEST_CASE("Better Carry Weight x10: a second test in the other placement replaces the first") {
    Fixture f;
    auto env = f.environment();
    auto request = f.installRequest();
    request.test = true;
    REQUIRE(install::storeMod(request, env).ok());
    REQUIRE(install::reportTestResult(env, "PPSA28000", "dac30f1aa5840955", install::TestResult::NoEffect).ok());
    // The same placement again is the same install.
    CHECK(install::storeMod(request, env).error().code == ErrorCode::AlreadyExists);
    request.placement = install::TestPlacement::PaksFolder;
    auto second = install::storeMod(request, env);
    REQUIRE(second.ok());
    auto state = install::loadTitleState(env, "PPSA28000");
    REQUIRE(state->mods.size() == 1);
    CHECK(state->mods[0].testPlacement == install::TestPlacement::PaksFolder);
    CHECK(state->mods[0].testResult == install::TestResult::Untested);
    REQUIRE(install::applyOverlay(kDawnwalker(f), env).ok());
    const auto backport = f.homebrew / "backports" / "PPSA28000";
    CHECK(fs::exists(backport / "dawnwalker/content/paks/00000000_bettercarryweightx10_p.utoc"));
    CHECK_FALSE(fs::exists(backport / "dawnwalker/content/paks/~mods"));
}
