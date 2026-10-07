// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/app/SystemCheck.hpp"

#include <cerrno>

#include <system_error>

#include <sys/stat.h>

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"

namespace akeno::app {

namespace fs = std::filesystem;

std::string_view toString(CheckStatus status) noexcept {
    switch (status) {
        case CheckStatus::Ok: return "OK";
        case CheckStatus::Warning: return "WARNING";
        case CheckStatus::Failed: return "FAILED";
        case CheckStatus::Skipped: return "SKIPPED";
    }
    return "?";
}

std::string_view toString(FeatureState state) noexcept {
    switch (state) {
        case FeatureState::Available: return "AVAILABLE";
        case FeatureState::Disabled: return "DISABLED";
        case FeatureState::NotImplemented: return "NOT YET IMPLEMENTED";
    }
    return "?";
}

namespace {

CheckResult make(CheckId id, std::string label, CheckStatus status, std::string summary, std::string detail = {}) {
    return CheckResult{id, std::move(label), status, std::move(summary), std::move(detail)};
}

const CheckResult* findIn(const std::vector<CheckResult>& checks, CheckId id) {
    for (const auto& check : checks) {
        if (check.id == id) return &check;
    }
    return nullptr;
}

bool isOk(const std::vector<CheckResult>& checks, CheckId id) {
    const CheckResult* check = findIn(checks, id);
    return check != nullptr && check->status == CheckStatus::Ok;
}

std::string failureReason(const std::vector<CheckResult>& checks, CheckId id) {
    const CheckResult* check = findIn(checks, id);
    if (check == nullptr) return "not checked";
    return check->label + ": " + check->summary;
}

}  // namespace

FeatureAvailability computeFeatures(const std::vector<CheckResult>& checks, const BuildFeatures& build) {
    FeatureAvailability features;

    if (isOk(checks, CheckId::ShadowMountApi)) {
        features.gameLibrary.state = FeatureState::Available;
    } else {
        features.gameLibrary.state = FeatureState::Disabled;
        features.gameLibrary.reason = failureReason(checks, CheckId::ShadowMountApi);
    }

    if (!build.modBrowsing) {
        features.modBrowsing.state = FeatureState::NotImplemented;
        features.modBrowsing.reason = "Online mod browsing is planned for Phase 2.";
    } else if (isOk(checks, CheckId::Networking)) {
        features.modBrowsing.state = FeatureState::Available;
    } else {
        features.modBrowsing.state = FeatureState::Disabled;
        features.modBrowsing.reason = failureReason(checks, CheckId::Networking);
    }

    if (!build.downloading) {
        features.downloading.state = FeatureState::NotImplemented;
        features.downloading.reason = "The download engine is planned for Phase 3.";
    } else if (!isOk(checks, CheckId::Networking)) {
        features.downloading.state = FeatureState::Disabled;
        features.downloading.reason = failureReason(checks, CheckId::Networking);
    } else if (!isOk(checks, CheckId::WritableStorage)) {
        features.downloading.state = FeatureState::Disabled;
        features.downloading.reason = failureReason(checks, CheckId::WritableStorage);
    } else {
        features.downloading.state = FeatureState::Available;
    }

    if (!build.installation) {
        features.installation.state = FeatureState::NotImplemented;
        features.installation.reason =
            "Overlay installation is not part of this build. No game data is modified.";
    } else {
        for (CheckId required : {CheckId::ShadowMountApi, CheckId::WritableStorage, CheckId::Database,
                                 CheckId::OverlayCapability}) {
            if (!isOk(checks, required)) {
                features.installation.state = FeatureState::Disabled;
                features.installation.reason = failureReason(checks, required);
                return features;
            }
        }
        features.installation.state = FeatureState::Available;
    }
    return features;
}

const CheckResult* SystemReport::find(CheckId id) const { return findIn(checks, id); }

std::string SystemReport::toText() const {
    std::string text;
    text += strings::concat("Akeno PS5 Mod Manager ", build::version(), " (", build::target(), ", ",
                            build::gitRevision(), ")\n");
    text += "System Check - " + generatedAt + "\n";
    text += "Platform: " + platformName + "\n\n";
    for (const auto& check : checks) {
        text += strings::concat(check.label, ": ", check.summary);
        if (check.status != CheckStatus::Ok) {
            text += strings::concat(" [", toString(check.status), "]");
        }
        text += "\n";
        if (!check.detail.empty()) {
            text += "    detail: " + check.detail + "\n";
        }
    }
    text += strings::concat("\nSAFE MODE: ", features.safeMode() ? "ON" : "OFF", "\n");
    for (const Feature* feature : features.all()) {
        text += strings::concat(feature->name, ": ", toString(feature->state), "\n");
    }
    bool headerWritten = false;
    for (const Feature* feature : features.all()) {
        if (feature->state == FeatureState::Available || feature->reason.empty()) continue;
        if (!headerWritten) {
            text += "\nReasons:\n";
            headerWritten = true;
        }
        text += strings::concat("  ", feature->name, ": ", feature->reason, "\n");
    }
    text += strings::concat("\nTesting status: ", build::testingStatus(), "\n");
    return text;
}

// ---------------------------------------------------------------- individual checks

CheckResult SystemChecker::checkFirmware() {
    platform::FirmwareInfo firmware = deps_.platform->firmware();
    if (!deps_.platform->isConsole()) {
        return make(CheckId::Firmware, "Firmware", CheckStatus::Skipped, firmware.display);
    }
    // Informational only: features are never gated on this number.
    return make(CheckId::Firmware, "Firmware", firmware.known ? CheckStatus::Ok : CheckStatus::Warning,
                firmware.display, firmware.known ? "" : strings::concat("raw value 0x", std::hex, firmware.raw));
}

CheckResult SystemChecker::checkHomebrewEnvironment() {
    platform::HomebrewEnvironment env = deps_.platform->probeHomebrewEnvironment(deps_.paths.root);
    std::string detail;
    for (const auto& component : env.components) {
        if (!detail.empty()) detail += "; ";
        detail += component;
    }
    if (!env.isConsole) {
        return make(CheckId::HomebrewEnvironment, "Homebrew environment", CheckStatus::Skipped, env.summary, detail);
    }
    return make(CheckId::HomebrewEnvironment, "Homebrew environment",
                env.dataWritable ? CheckStatus::Ok : CheckStatus::Failed, env.summary, detail);
}

std::pair<CheckResult, CheckResult> SystemChecker::checkShadowMount() {
    if (deps_.gameProvider == nullptr) {
        std::string reason = deps_.gameProviderError ? deps_.gameProviderError->message : "not configured";
        std::string detail = deps_.gameProviderError ? deps_.gameProviderError->detail : "";
        return {make(CheckId::ShadowMount, "ShadowMount", CheckStatus::Failed, reason, detail),
                make(CheckId::ShadowMountApi, "ShadowMount API", CheckStatus::Failed, "not connected - " + reason)};
    }
    auto status = deps_.gameProvider->probe();
    if (!status) {
        const Error& error = status.error();
        return {make(CheckId::ShadowMount, "ShadowMount", CheckStatus::Failed, "not detected", error.detail),
                make(CheckId::ShadowMountApi, "ShadowMount API", CheckStatus::Failed,
                     "not connected - " + error.message, error.detail)};
    }
    CheckResult detected = make(CheckId::ShadowMount, "ShadowMount", CheckStatus::Ok,
                                "detected (version " + status->providerVersion + ")");
    if (!status->available) {
        return {detected, make(CheckId::ShadowMountApi, "ShadowMount API", CheckStatus::Failed, status->summary)};
    }
    std::string detail = status->supportsIcons ? "" : "game icons are not offered; placeholders will be shown";
    return {detected, make(CheckId::ShadowMountApi, "ShadowMount API", CheckStatus::Ok, status->summary, detail)};
}

CheckResult SystemChecker::checkWritableStorage() {
    const std::string label = "Writable data storage";
    for (const fs::path& directory : deps_.paths.layout()) {
        auto created = deps_.fs->createDirectories(directory);
        if (!created) {
            return make(CheckId::WritableStorage, label, CheckStatus::Failed, created.error().message,
                        created.error().detail);
        }
    }
    const fs::path probe = deps_.paths.cache() / "write-probe.txt";
    const std::string payload = "akeno write probe " + strings::utcTimestamp();
    auto written = deps_.fs->writeFileAtomic(probe, payload);
    if (!written) {
        return make(CheckId::WritableStorage, label, CheckStatus::Failed, written.error().message,
                    written.error().detail);
    }
    auto readBack = security::readFileBounded(probe, 4096);
    (void)deps_.fs->removeFile(probe);
    if (!readBack || readBack.value() != payload) {
        return make(CheckId::WritableStorage, label, CheckStatus::Failed,
                    "the test file could not be read back correctly");
    }
    auto space = security::queryStorageSpace(deps_.paths.root);
    if (!space) {
        return make(CheckId::WritableStorage, label, CheckStatus::Warning, "writable; free space unknown",
                    space.error().detail);
    }
    std::string summary = "OK - " + strings::formatBytes(space->availableBytes) + " free";
    if (space->availableBytes < limits::kStorageSafetyReserveBytes) {
        return make(CheckId::WritableStorage, label, CheckStatus::Warning,
                    summary + " (below the " + strings::formatBytes(limits::kStorageSafetyReserveBytes) +
                        " safety reserve)",
                    deps_.paths.root.string());
    }
    return make(CheckId::WritableStorage, label, CheckStatus::Ok, summary, deps_.paths.root.string());
}

CheckResult SystemChecker::checkNetworking() {
    const std::string label = "Networking";
    if (deps_.http == nullptr || deps_.networkProbeUrl.empty()) {
        return make(CheckId::Networking, label, CheckStatus::Skipped, "not configured");
    }
    network::HttpRequest request;
    request.method = network::HttpMethod::Head;
    request.url = deps_.networkProbeUrl;
    request.connectTimeoutMs = 5000;
    request.totalTimeoutMs = 8000;
    request.maxRedirects = 0;
    request.maxResponseBytes = 64 * 1024;
    auto response = deps_.http->send(request);
    if (!response) {
        return make(CheckId::Networking, label, CheckStatus::Failed, response.error().message,
                    response.error().detail);
    }
    // Any HTTP answer proves DNS, TCP and a verified TLS handshake.
    return make(CheckId::Networking, label, CheckStatus::Ok, "OK (secure connection verified)",
                strings::concat(deps_.networkProbeUrl, " answered HTTP ", response->status));
}

CheckResult SystemChecker::checkDatabase() {
    const std::string label = "Database";
    if (deps_.db == nullptr) {
        std::string reason = deps_.databaseError ? deps_.databaseError->message : "not opened";
        std::string detail = deps_.databaseError ? deps_.databaseError->detail : "";
        return make(CheckId::Database, label, CheckStatus::Failed, reason, detail);
    }
    std::lock_guard<std::mutex> lock(deps_.db->mutex());
    auto check = deps_.db->quickCheck();
    if (!check) {
        return make(CheckId::Database, label, CheckStatus::Failed, check.error().message, check.error().detail);
    }
    if (check.value() != "ok") {
        return make(CheckId::Database, label, CheckStatus::Failed, "integrity check reported a problem",
                    check.value());
    }
    auto version = deps_.db->userVersion();
    return make(CheckId::Database, label, CheckStatus::Ok,
                version ? strings::concat("OK (schema v", version.value(), ")") : "OK");
}

CheckResult SystemChecker::checkOverlayCapability(const CheckResult& shadowMountApi) {
    const std::string label = "Overlay capability";
    // Facts gathered now so hardware reports already contain them; activation itself is a
    // Phase 5 feature and is not part of this build.
    std::string facts;
    const fs::path a = deps_.paths.staging() / "link-probe-a";
    const fs::path b = deps_.paths.staging() / "link-probe-b";
    (void)deps_.fs->removeFile(a);
    (void)deps_.fs->removeFile(b);
    // Records why a probe failed, so a hardware report shows the cause, not only the result.
    bool hardLinks = false;
    std::string why;
    auto written = deps_.fs->writeFileAtomic(a, "probe");
    if (!written) {
        why = "could not write the probe file: " + written.error().describe();
    } else if (auto linked = deps_.fs->createHardLink(a, b); !linked) {
        why = "link() failed: " + linked.error().describe();
    } else {
        struct stat info {};
        if (::lstat(b.c_str(), &info) != 0) {
            why = strings::concat("the link cannot be read back (errno ", errno, ")");
        } else if (info.st_nlink != 2) {
            why = strings::concat("the link count is ", info.st_nlink, " instead of 2");
        } else {
            hardLinks = true;
        }
    }
    for (const fs::path& probe : {a, b}) {
        if (auto removed = deps_.fs->removeFile(probe); !removed) {
            logging::logger().warn("syscheck", "could not remove a probe file: " + removed.error().describe());
        }
    }
    hardLinks_ = hardLinks;
    facts = strings::concat("hard links in app storage: ", hardLinks ? "supported" : "not supported (" + why + ")",
                            "; ShadowMount API: ", shadowMountApi.status == CheckStatus::Ok ? "connected" : "unavailable");
    if (!deps_.build.installation) {
        return make(CheckId::OverlayCapability, label, CheckStatus::Warning, "not implemented in this build (Phase 5)",
                    facts);
    }
    if (shadowMountApi.status != CheckStatus::Ok) {
        return make(CheckId::OverlayCapability, label, CheckStatus::Failed, "ShadowMountPlus is required", facts);
    }
    // Activation renames a folder from Akeno's storage into the backports folder: both must be
    // real folders on the same drive. The backports folder itself may still be missing.
    const fs::path& backports = deps_.backportsRoot;
    struct stat where {};
    const bool exists = ::lstat(backports.c_str(), &where) == 0;
    if (exists && !S_ISDIR(where.st_mode)) {
        return make(CheckId::OverlayCapability, label, CheckStatus::Failed,
                    "the ShadowMountPlus backports location is not a folder", facts + "; " + backports.string());
    }
    const fs::path probe = exists ? backports : backports.parent_path();
    struct stat target {};
    struct stat app {};
    if (::lstat(probe.c_str(), &target) != 0 || !S_ISDIR(target.st_mode)) {
        return make(CheckId::OverlayCapability, label, CheckStatus::Failed,
                    "the ShadowMountPlus folder " + probe.string() + " was not found", facts);
    }
    if (::stat(deps_.paths.staging().c_str(), &app) != 0 || app.st_dev != target.st_dev) {
        return make(CheckId::OverlayCapability, label, CheckStatus::Failed,
                    "the backports folder is not on the same drive as Akeno's storage", facts + "; " + backports.string());
    }
    facts += "; overlays: " + std::string(hardLinks ? "hard links possible, copies used" : "copies") + " into " +
             backports.string() + "/<TITLE_ID>";
    return make(CheckId::OverlayCapability, label, CheckStatus::Ok, "OK", facts);
}

SystemReport SystemChecker::run(const std::function<void(const CheckResult&)>& onResult) {
    SystemReport report;
    report.generatedAt = strings::utcTimestamp();
    report.platformName = std::string(deps_.platform->name());
    auto add = [&](CheckResult result) {
        logging::logger().info("syscheck", strings::concat(result.label, ": ", result.summary, " [",
                                                           toString(result.status), "]",
                                                           result.detail.empty() ? "" : " - " + result.detail));
        if (onResult) onResult(result);
        report.checks.push_back(std::move(result));
    };
    add(checkFirmware());
    add(checkHomebrewEnvironment());
    auto [shadowMount, shadowMountApi] = checkShadowMount();
    add(shadowMount);
    add(shadowMountApi);
    add(checkWritableStorage());
    add(checkNetworking());
    add(checkDatabase());
    add(checkOverlayCapability(*report.find(CheckId::ShadowMountApi)));
    report.features = computeFeatures(report.checks, deps_.build);
    report.hardLinksSupported = hardLinks_;
    logging::logger().info("syscheck", strings::concat("safe mode: ", report.features.safeMode() ? "ON" : "OFF"));
    return report;
}

}  // namespace akeno::app
