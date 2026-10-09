// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/install/TestReports.hpp"

#include <algorithm>

#include "akeno/core/Strings.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::install {

std::string_view toString(TestPlacement placement) noexcept {
    switch (placement) {
        case TestPlacement::ModsFolder: return "mods-folder";
        case TestPlacement::PaksFolder: return "paks-folder";
    }
    return "mods-folder";
}

std::optional<TestPlacement> parseTestPlacement(std::string_view text) noexcept {
    for (auto value : {TestPlacement::ModsFolder, TestPlacement::PaksFolder}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view toString(TestResult result) noexcept {
    switch (result) {
        case TestResult::Untested: return "untested";
        case TestResult::Works: return "works";
        case TestResult::NoEffect: return "no-effect";
        case TestResult::Crashed: return "crashed";
    }
    return "untested";
}

std::optional<TestResult> parseTestResult(std::string_view text) noexcept {
    for (auto value : {TestResult::Untested, TestResult::Works, TestResult::NoEffect, TestResult::Crashed}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view describe(TestResult result) noexcept {
    switch (result) {
        case TestResult::Untested: return "not reported yet";
        case TestResult::Works: return "works";
        case TestResult::NoEffect: return "no effect";
        case TestResult::Crashed: return "crashed the game";
    }
    return "not reported yet";
}

namespace {

bool sameMod(const TestRecord& record, const compatibility::ModFacts& mod) {
    return !record.modId.empty() && record.provider == mod.provider && record.modId == mod.modId &&
           record.modVersion == mod.modVersion;
}

bool sameGame(const TestRecord& record, const compatibility::GameFacts& game) {
    return !record.gameVersion.empty() && record.gameVersion == game.version;
}

std::string day(const std::string& timestamp) { return timestamp.substr(0, 10); }

class TestReportAdapter final : public compatibility::IGameAdapter {
public:
    TestReportAdapter(std::string titleId, std::vector<TestRecord> records)
        : titleId_(std::move(titleId)), records_(std::move(records)) {}

    std::string id() const override { return "your test reports"; }
    bool appliesTo(std::string_view titleId) const override { return titleId == titleId_; }
    compatibility::EvidenceSource evidenceSource() const override { return compatibility::EvidenceSource::Report; }

    std::vector<compatibility::LoadingConvention> conventions() const override {
        std::vector<compatibility::LoadingConvention> result;
        for (const auto& record : records_) {
            if (record.result != TestResult::Works || record.gameVersion.empty() || record.directory.empty()) continue;
            compatibility::LoadingConvention convention;
            convention.directory = record.directory;
            convention.extensions = {"pak", "utoc", "ucas", "sig"};
            convention.verifiedVersions = {record.gameVersion};
            convention.evidence = "you reported that " + record.name + " " + record.modVersion + " worked on " +
                                  day(record.reportedAt) + " after a test install";
            // One working mod shows the folder is loaded, not that other PC packages work.
            convention.pcCookedAssetsVerified = false;
            result.push_back(std::move(convention));
        }
        return result;
    }

    bool modVerified(const compatibility::ModFacts& mod, const compatibility::GameFacts& game) const override {
        return std::any_of(records_.begin(), records_.end(), [&](const TestRecord& record) {
            return record.result == TestResult::Works && sameMod(record, mod) && sameGame(record, game);
        });
    }

    std::optional<std::string> knownProblem(const compatibility::ModFacts& mod,
                                            const compatibility::GameFacts& game) const override {
        for (const auto& record : records_) {
            if (record.result != TestResult::Crashed || !sameMod(record, mod) || !sameGame(record, game)) continue;
            return "You reported on " + day(record.reportedAt) + " that " + record.name + " " + record.modVersion +
                   " crashed this game (version " + record.gameVersion + "), so it is not offered for another test on "
                   "this game version.";
        }
        return std::nullopt;
    }

private:
    std::string titleId_;
    std::vector<TestRecord> records_;
};

}  // namespace

std::shared_ptr<const compatibility::IGameAdapter> makeTestReportAdapter(std::string titleId,
                                                                         std::vector<TestRecord> records) {
    return std::make_shared<TestReportAdapter>(std::move(titleId), std::move(records));
}

compatibility::Registry registryWithReports(const std::string& titleId, const std::vector<TestRecord>& records) {
    compatibility::Registry registry;
    const compatibility::Registry& builtin = compatibility::Registry::builtin();
    for (const auto& adapter : builtin.adapters()) registry.addAdapter(adapter);
    for (const auto& conversion : builtin.conversions()) registry.addConversion(conversion);
    const bool relevant = std::any_of(records.begin(), records.end(), [](const TestRecord& record) {
        return record.result == TestResult::Works || record.result == TestResult::Crashed;
    });
    if (relevant) registry.addAdapter(makeTestReportAdapter(titleId, records));
    return registry;
}

std::string evidenceStamp(const std::vector<TestRecord>& records) {
    std::string stamp;
    for (const auto& record : records) {
        if (record.result != TestResult::Works && record.result != TestResult::Crashed) continue;
        stamp += strings::concat(record.provider, "/", record.modId, "@", record.modVersion, ":", toString(record.result),
                                 ":", record.gameVersion, ":", record.directory, ";");
    }
    return stamp.empty() ? std::string() : security::sha256Hex(stamp);
}

}  // namespace akeno::install
