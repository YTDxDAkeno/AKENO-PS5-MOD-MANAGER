// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "ArchiveTestSupport.hpp"
#include "TestSupport.hpp"
#include "UnrealTestSupport.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/security/Sha256.hpp"

using namespace akeno;
using namespace akeno::mods;
using providers::CompatibilityStatus;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    test::TempDir dir;
    AppPaths paths;
    std::unique_ptr<security::SafeFs> fs;
    std::unique_ptr<OperationJournal> journal;
    std::uint64_t available = 100ull * 1024 * 1024 * 1024;

    Fixture() {
        paths.root = dir.path() / "data";
        for (const auto& directory : paths.layout()) fs::create_directories(directory);
        fs = std::make_unique<security::SafeFs>(security::WriteGuard::create({paths.root}).value());
        journal = std::make_unique<OperationJournal>(paths.operationJournal(), *fs);
    }

    ModCheckEnvironment environment(bool interrupted = false) {
        auto query = [this](const fs::path&) -> Result<security::StorageSpace> {
            return security::StorageSpace{available * 2, available};
        };
        return ModCheckEnvironment{*fs, paths, *journal, interrupted, limits::kStorageSafetyReserveBytes, {}, query};
    }

    ModCheckRequest request(const std::string& id, const std::vector<test::EntrySpec>& entries) {
        ModCheckRequest r;
        r.downloadId = id;
        r.archive = paths.downloads() / (id + ".zip");
        REQUIRE(test::writeArchive(r.archive, ArchiveFormat::Zip, entries));
        r.format = ArchiveFormat::Zip;
        r.mod = {"akeno-catalogue", "example-blade/crimson-outfit"};
        r.displayName = "Crimson Outfit Recolour";
        r.modVersion = "1.2.0";
        r.titleId = "PPSA90001";
        r.sourceType = games::SourceType::Folder;
        r.archiveRoot = "Crimson";
        r.targetPrefix = "";
        r.catalogueStatus = CompatibilityStatus::Verified;
        r.catalogueInstallable = true;
        return r;
    }

    std::size_t stagingEntries() const {
        std::size_t count = 0;
        for (const auto& entry : fs::directory_iterator(paths.staging())) {
            (void)entry;
            ++count;
        }
        return count;
    }
};

const std::string kCrimsonPak = test::legacyPak({{"ExampleBlade/Content/Outfits/Crimson.uasset", std::string(4000, 'p')}});
const std::vector<test::EntrySpec> kGoodMod{
    {"Crimson/Content/Paks/~mods/crimson.pak", kCrimsonPak, AE_IFREG, "", ""},
    {"Crimson/Content/crimson.ini", "[a]\n", AE_IFREG, "", ""},
    {"README.txt", "Read me", AE_IFREG, "", ""},
};

}  // namespace

TEST_CASE("a mod is checked in staging, and staging and journal are gone afterwards") {
    Fixture f;
    auto env = f.environment();
    std::vector<CheckPhase> phases;
    auto report = runModCheck(f.request("0123456789abcdef", kGoodMod), env, nullptr,
                              [&](CheckPhase phase, double) {
                                  if (phases.empty() || phases.back() != phase) phases.push_back(phase);
                              });
    REQUIRE(report.ok());
    CHECK(phases == std::vector<CheckPhase>{CheckPhase::Inspecting, CheckPhase::Extracting, CheckPhase::Analysing,
                                            CheckPhase::CleaningUp});
    CHECK(f.stagingEntries() == 0);
    CHECK_FALSE(fs::exists(f.paths.operationJournal()));
    CHECK(report->archiveFiles == 3);
    CHECK(report->analysis.installCount == 2);
    CHECK(report->analysis.installable);
    CHECK(report->analysis.status == CompatibilityStatus::Verified);
    CHECK(report->analysis.files[0].sha256 == security::sha256Hex(kCrimsonPak));
    CHECK(report->assessment.outcome == compatibility::Outcome::VerifiedPs5);
    CHECK(report->assessment.activationAllowed);
    CHECK(report->layout.rule == "manifest");
    CHECK(report->unreal.detected);
    CHECK(report->unreal.maxPakVersion == 8);

    // Stored and read back.
    REQUIRE(saveReport(*f.fs, f.paths, report.value()).ok());
    auto loaded = loadReport(f.paths, "0123456789abcdef");
    REQUIRE(loaded.ok());
    CHECK(loaded->displayName == "Crimson Outfit Recolour");
    CHECK(loaded->analysis.installCount == 2);
    CHECK(loaded->analysis.installBytes == kCrimsonPak.size() + 4);
    CHECK(loaded->assessment.outcome == compatibility::Outcome::VerifiedPs5);
    CHECK(loaded->layout.archiveRoot == "Crimson");
    CHECK(loaded->unreal.containedPackages == std::vector<std::string>{"/Game/Outfits/Crimson"});
    CHECK(loaded->analysis.files.size() == 3);
    CHECK(loaded->analysis.installable);

    completeReport(loaded.value(), f.paths);
    REQUIRE(loaded->plan.has_value());
    CHECK(loaded->plan->files == 2);
    CHECK(loaded->plan->executable);
    CHECK(loaded->plan->mappingConfidence == "established");
    CHECK(loaded->conflicts.empty());
}

TEST_CASE("conflicts with other checked mods of the same game") {
    Fixture f;
    auto env = f.environment();
    auto first = runModCheck(f.request("aaaaaaaaaaaaaaaa", kGoodMod), env);
    REQUIRE(first.ok());
    REQUIRE(saveReport(*f.fs, f.paths, first.value()).ok());
    auto secondRequest = f.request("bbbbbbbbbbbbbbbb", {{"Crimson/Content/Paks/~MODS/CRIMSON.pak", "other", AE_IFREG, "", ""}});
    secondRequest.displayName = "Another Outfit";
    auto second = runModCheck(secondRequest, env);
    REQUIRE(second.ok());
    completeReport(second.value(), f.paths);
    REQUIRE(second->conflicts.size() == 1);
    CHECK(second->conflicts[0].otherName == "Crimson Outfit Recolour");
    CHECK(second->conflicts[0].count == 1);

    // Other games do not count.
    auto otherGame = f.request("cccccccccccccccc", kGoodMod);
    otherGame.titleId = "PPSA90002";
    auto third = runModCheck(otherGame, env);
    REQUIRE(third.ok());
    completeReport(third.value(), f.paths);
    CHECK(third->conflicts.empty());
}

TEST_CASE("a hostile archive is refused and nothing stays in staging") {
    Fixture f;
    auto env = f.environment();
    auto report = runModCheck(f.request("0123456789abcdef", {{"../escape.txt", "x", AE_IFREG, "", ""}}), env);
    REQUIRE_FALSE(report.ok());
    CHECK(report.error().code == ErrorCode::SafetyViolation);
    CHECK(f.stagingEntries() == 0);
    CHECK_FALSE(fs::exists(f.paths.operationJournal()));
    CHECK_FALSE(fs::exists(f.dir.path() / "data" / "escape.txt"));
}

TEST_CASE("checks wait for an interrupted operation and need free space") {
    Fixture f;
    auto interrupted = f.environment(true);
    auto refused = runModCheck(f.request("0123456789abcdef", kGoodMod), interrupted);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Busy);

    f.available = limits::kStorageSafetyReserveBytes + 100;
    auto env = f.environment();
    auto full = runModCheck(f.request("0123456789abcdef", kGoodMod), env);
    REQUIRE_FALSE(full.ok());
    CHECK(full.error().code == ErrorCode::NoSpace);
    CHECK(f.stagingEntries() == 0);
}

TEST_CASE("a cancelled check cleans up") {
    Fixture f;
    auto env = f.environment();
    CancellationToken cancel;
    cancel.cancel();
    auto result = runModCheck(f.request("0123456789abcdef", kGoodMod), env, &cancel);
    REQUIRE_FALSE(result.ok());
    CHECK(f.stagingEntries() == 0);
    CHECK_FALSE(fs::exists(f.paths.operationJournal()));
}

TEST_CASE("stored reports are validated when read") {
    Fixture f;
    auto env = f.environment();
    auto report = runModCheck(f.request("0123456789abcdef", kGoodMod), env);
    REQUIRE(report.ok());
    REQUIRE(saveReport(*f.fs, f.paths, report.value()).ok());
    const fs::path path = reportPath(f.paths, "0123456789abcdef");
    std::string text = security::readFileBounded(path, 1 << 20).value();

    auto tamper = [&](const std::string& from, const std::string& to) {
        std::string changed = text;
        const auto at = changed.find(from);
        REQUIRE(at != std::string::npos);
        changed.replace(at, from.size(), to);
        test::writeText(path, changed);
        return loadReport(f.paths, "0123456789abcdef");
    };
    CHECK_FALSE(tamper("Content/crimson.ini\"", "../../evil.ini\"").ok());
    CHECK_FALSE(tamper("\"status\": \"verified\"", "\"status\": \"great\"").ok());
    CHECK_FALSE(tamper("\"kind\": \"asset\"", "\"kind\": \"rocket\"").ok());
    test::writeText(path, "{ not json");
    CHECK_FALSE(loadReport(f.paths, "0123456789abcdef").ok());
    CHECK_FALSE(loadReport(f.paths, "../../etc/passwd").ok());
    CHECK_FALSE(loadReport(f.paths, "ffffffffffffffff").ok());
}
