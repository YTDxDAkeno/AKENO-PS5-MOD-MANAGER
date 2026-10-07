// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/compatibility/CompatibilityRules.hpp"

using namespace akeno;
using namespace akeno::compatibility;
using providers::CompatibilityStatus;

namespace {

mods::ModManifest manifest(mods::ClaimedStatus claimed, std::vector<std::string> versions) {
    mods::ModManifest m;
    m.id = "x";
    m.platform = "ps5";
    m.titleIds = {"PPSA01234"};
    m.gameVersions = std::move(versions);
    m.claimed = claimed;
    m.modType = mods::ModType::AssetReplacement;
    m.modTypeText = "asset-replacement";
    m.installMethod = "shadowmount-overlay";
    m.format = mods::ArchiveFormat::Zip;
    return m;
}

InstalledGame installed(std::string version) { return InstalledGame{"PPSA01234", std::move(version)}; }

}  // namespace

TEST_CASE("verified needs the installed version to be listed") {
    auto m = manifest(mods::ClaimedStatus::Verified, {"1.010", "1.011"});
    auto ok = evaluateManifest(m, installed("1.011"));
    CHECK(ok.status == CompatibilityStatus::Verified);
    CHECK(ok.risk == Risk::Low);
    CHECK(ok.installable);
    CHECK_FALSE(ok.needsConfirmation);

    auto other = evaluateManifest(m, installed("1.020"));
    CHECK(other.status == CompatibilityStatus::Experimental);
    CHECK(other.needsConfirmation);
    REQUIRE_FALSE(other.reasons.empty());
    CHECK(other.reasons[0] == "Verified for game version 1.010; installed version is 1.020.");

    CHECK(evaluateManifest(m, installed("")).status == CompatibilityStatus::Experimental);
    CHECK(evaluateManifest(manifest(mods::ClaimedStatus::Verified, {}), installed("1.011")).status ==
          CompatibilityStatus::Experimental);
}

TEST_CASE("a title ID mismatch is incompatible whatever the claim") {
    auto m = manifest(mods::ClaimedStatus::Verified, {"1.011"});
    auto r = evaluateManifest(m, InstalledGame{"PPSA99999", "1.011"});
    CHECK(r.status == CompatibilityStatus::Incompatible);
    CHECK_FALSE(r.installable);
}

TEST_CASE("PC-only, incompatible and non-PS5 mods are never installable") {
    CHECK(evaluateManifest(manifest(mods::ClaimedStatus::PcOnly, {}), installed("1.0")).status ==
          CompatibilityStatus::PcOnly);
    CHECK_FALSE(evaluateManifest(manifest(mods::ClaimedStatus::PcOnly, {}), installed("1.0")).installable);
    CHECK(evaluateManifest(manifest(mods::ClaimedStatus::Incompatible, {}), installed("1.0")).status ==
          CompatibilityStatus::Incompatible);
    auto pc = manifest(mods::ClaimedStatus::Verified, {"1.0"});
    pc.platform = "pc";
    CHECK(evaluateManifest(pc, installed("1.0")).status == CompatibilityStatus::Incompatible);
}

TEST_CASE("likely, experimental and unknown claims") {
    CHECK(evaluateManifest(manifest(mods::ClaimedStatus::Likely, {}), installed("1.0")).status ==
          CompatibilityStatus::Likely);
    CHECK(evaluateManifest(manifest(mods::ClaimedStatus::Likely, {"2.0"}), installed("1.0")).status ==
          CompatibilityStatus::Experimental);
    CHECK(evaluateManifest(manifest(mods::ClaimedStatus::Experimental, {}), installed("1.0")).status ==
          CompatibilityStatus::Experimental);
    auto unknown = evaluateManifest(manifest(mods::ClaimedStatus::Unknown, {}), installed("1.0"));
    CHECK(unknown.status == CompatibilityStatus::Unknown);
    CHECK_FALSE(unknown.installable);
}

TEST_CASE("a game that is not installed can be browsed but not installed") {
    auto r = evaluateManifest(manifest(mods::ClaimedStatus::Verified, {"1.0"}), std::nullopt);
    CHECK(r.status == CompatibilityStatus::Experimental);
    CHECK_FALSE(r.installable);
}

TEST_CASE("unsupported mod types and formats are not installable") {
    auto script = manifest(mods::ClaimedStatus::Verified, {"1.0"});
    script.modType = mods::ModType::Other;
    script.modTypeText = "script";
    auto r = evaluateManifest(script, installed("1.0"));
    CHECK(r.status == CompatibilityStatus::Verified);
    CHECK_FALSE(r.installable);
    auto sevenZip = manifest(mods::ClaimedStatus::Verified, {"1.0"});
    sevenZip.format = mods::ArchiveFormat::SevenZip;
    CHECK_FALSE(evaluateManifest(sevenZip, installed("1.0")).installable);
}

TEST_CASE("list labels follow the same version rule") {
    mods::ModSummaryEntry entry;
    entry.claimed = mods::ClaimedStatus::Verified;
    entry.gameVersions = {"1.011"};
    CHECK(summaryLabel(entry, installed("1.011")) == CompatibilityStatus::Verified);
    CHECK(summaryLabel(entry, installed("1.020")) == CompatibilityStatus::Experimental);
    CHECK(summaryLabel(entry, std::nullopt) == CompatibilityStatus::Experimental);
    entry.claimed = mods::ClaimedStatus::Likely;
    CHECK(summaryLabel(entry, installed("1.011")) == CompatibilityStatus::Likely);
    CHECK(summaryLabel(entry, installed("2.0")) == CompatibilityStatus::Experimental);
    entry.gameVersions.clear();
    CHECK(summaryLabel(entry, installed("2.0")) == CompatibilityStatus::Likely);
}
