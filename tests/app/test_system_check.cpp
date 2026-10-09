// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/app/SystemCheck.hpp"
#include "akeno/database/Migrations.hpp"

using namespace akeno;
using namespace akeno::app;
namespace fs = std::filesystem;

namespace {

CheckResult check(CheckId id, CheckStatus status) { return CheckResult{id, "label", status, "summary", ""}; }

std::vector<CheckResult> allOk() {
    return {check(CheckId::Firmware, CheckStatus::Ok),          check(CheckId::HomebrewEnvironment, CheckStatus::Ok),
            check(CheckId::ShadowMount, CheckStatus::Ok),       check(CheckId::ShadowMountApi, CheckStatus::Ok),
            check(CheckId::WritableStorage, CheckStatus::Ok),   check(CheckId::Networking, CheckStatus::Ok),
            check(CheckId::Database, CheckStatus::Ok),          check(CheckId::OverlayCapability, CheckStatus::Ok)};
}

struct CheckerFixture {
    test::TempDir dir;
    AppPaths paths{dir.path() / "akeno"};
    security::SafeFs safeFs{security::WriteGuard::create({paths.root}).value()};
    test::FakePlatform platform;
    test::FakeGameProvider provider;
    test::MockHttpClient http;
    std::unique_ptr<database::Database> db;

    CheckerFixture() {
        auto opened = database::Database::openInMemory();
        REQUIRE(opened.ok());
        db = std::move(opened).value();
        REQUIRE(database::migrate(*db, database::builtinMigrations(), {}).ok());
        http.respond(network::HttpMethod::Head, "https://probe.example/", 301, "");
    }

    SystemCheckDependencies deps() {
        SystemCheckDependencies d;
        d.platform = &platform;
        d.gameProvider = &provider;
        d.http = &http;
        d.fs = &safeFs;
        d.paths = paths;
        d.db = db.get();
        d.networkProbeUrl = "https://probe.example/";
        return d;
    }
};

}  // namespace

TEST_CASE("this build never reports installation as available") {
    auto features = computeFeatures(allOk(), BuildFeatures{});
    CHECK(features.gameLibrary.state == FeatureState::Available);
    CHECK(features.modBrowsing.state == FeatureState::NotImplemented);
    CHECK(features.downloading.state == FeatureState::NotImplemented);
    CHECK(features.installation.state == FeatureState::NotImplemented);
    CHECK(features.safeMode());
}

TEST_CASE("feature gating uses capabilities") {
    BuildFeatures everything{true, true, true};
    CHECK_FALSE(computeFeatures(allOk(), everything).safeMode());

    auto checks = allOk();
    checks[3].status = CheckStatus::Failed;  // ShadowMount API
    auto features = computeFeatures(checks, everything);
    CHECK(features.gameLibrary.state == FeatureState::Disabled);
    CHECK(features.installation.state == FeatureState::Disabled);
    CHECK(features.safeMode());
    CHECK(features.modBrowsing.state == FeatureState::Available);

    checks = allOk();
    checks[5].status = CheckStatus::Failed;  // networking
    features = computeFeatures(checks, everything);
    CHECK(features.modBrowsing.state == FeatureState::Disabled);
    CHECK(features.downloading.state == FeatureState::Disabled);
    CHECK(features.installation.state == FeatureState::Available);  // offline installs are fine

    checks = allOk();
    checks[7].status = CheckStatus::Warning;  // overlay capability
    CHECK(computeFeatures(checks, everything).installation.state == FeatureState::Disabled);
}

TEST_CASE("firmware never gates anything") {
    BuildFeatures everything{true, true, true};
    auto checks = allOk();
    checks[0].status = CheckStatus::Warning;  // unknown firmware
    CHECK_FALSE(computeFeatures(checks, everything).safeMode());
}

TEST_CASE("full system check with a healthy (mocked) environment") {
    CheckerFixture f;
    SystemChecker checker(f.deps());
    std::vector<CheckId> progress;
    SystemReport report = checker.run([&](const CheckResult& r) { progress.push_back(r.id); });
    CHECK(progress.size() == 8);
    CHECK(report.find(CheckId::ShadowMountApi)->status == CheckStatus::Ok);
    CHECK(report.find(CheckId::WritableStorage)->status != CheckStatus::Failed);
    // Every report says whether files below an opened folder can be read (mod checks need it).
    CHECK(report.find(CheckId::WritableStorage)->detail.find("folder-relative file access: OK") != std::string::npos);
    CHECK(report.find(CheckId::Networking)->status == CheckStatus::Ok);
    CHECK(report.find(CheckId::Database)->status == CheckStatus::Ok);
    CHECK(report.find(CheckId::Database)->summary ==
          "OK (schema v" + std::to_string(akeno::database::latestSchemaVersion()) + ")");
    CHECK(report.find(CheckId::OverlayCapability)->status == CheckStatus::Warning);
    CHECK(report.find(CheckId::OverlayCapability)->detail.find("hard links in app storage: supported") !=
          std::string::npos);
    CHECK(report.find(CheckId::Firmware)->status == CheckStatus::Skipped);
    CHECK(report.features.safeMode());
    CHECK(report.features.gameLibrary.state == FeatureState::Available);
    // Probes must not leave files behind.
    CHECK_FALSE(fs::exists(f.paths.cache() / "write-probe.txt"));
    CHECK_FALSE(fs::exists(f.paths.staging() / "link-probe-a"));

    std::string text = report.toText();
    CHECK(text.find("SAFE MODE: ON") != std::string::npos);
    CHECK(text.find("Installation: NOT YET IMPLEMENTED") != std::string::npos);
    CHECK(text.find("Testing status: unit tested on the host") != std::string::npos);
    CHECK(text.find("not hardware tested") != std::string::npos);
}

TEST_CASE("system check reports ShadowMount and database failures with reasons") {
    CheckerFixture f;
    f.provider.status = makeError(ErrorCode::Unavailable, "ShadowMountPlus is not answering on port 10101.");
    auto deps = f.deps();
    deps.db = nullptr;
    deps.databaseError = makeError(ErrorCode::Unsupported, "The database was created by a newer version.");
    SystemChecker checker(deps);
    SystemReport report = checker.run();
    CHECK(report.find(CheckId::ShadowMount)->status == CheckStatus::Failed);
    CHECK(report.find(CheckId::ShadowMount)->summary == "not detected");
    CHECK(report.find(CheckId::ShadowMountApi)->summary.find("not answering") != std::string::npos);
    CHECK(report.find(CheckId::ShadowMountApi)->status == CheckStatus::Failed);
    CHECK(report.find(CheckId::Database)->status == CheckStatus::Failed);
    CHECK(report.features.gameLibrary.state == FeatureState::Disabled);
    CHECK_FALSE(report.features.gameLibrary.reason.empty());
}

TEST_CASE("console firmware is shown but only informational") {
    CheckerFixture f;
    f.platform.console = true;
    f.platform.firmwareInfo = platform::FirmwareInfo{true, 0x12200000, "12.20"};
    SystemChecker checker(f.deps());
    auto firmware = checker.checkFirmware();
    CHECK(firmware.status == CheckStatus::Ok);
    CHECK(firmware.summary == "12.20");
    auto env = checker.checkHomebrewEnvironment();
    CHECK(env.status == CheckStatus::Ok);
}

TEST_CASE("network failure is reported, not hidden") {
    CheckerFixture f;
    f.http.fail(network::HttpMethod::Head, "https://probe.example/",
                makeError(ErrorCode::TlsError, "The secure connection could not be verified."));
    SystemChecker checker(f.deps());
    auto result = checker.checkNetworking();
    CHECK(result.status == CheckStatus::Failed);
    CHECK(result.summary.find("could not be verified") != std::string::npos);
}
