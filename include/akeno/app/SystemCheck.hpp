// SPDX-License-Identifier: GPL-3.0-or-later
// Startup environment check. Features are enabled from detected capabilities, never from the
// firmware number (docs/safety-model.md §8).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "akeno/core/AppPaths.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/games/IGameDiscoveryProvider.hpp"
#include "akeno/network/Http.hpp"
#include "akeno/platform/Platform.hpp"
#include "akeno/security/SafeFs.hpp"

namespace akeno::app {

enum class CheckStatus { Ok, Warning, Failed, Skipped };
std::string_view toString(CheckStatus status) noexcept;

enum class CheckId {
    Firmware,
    HomebrewEnvironment,
    ShadowMount,
    ShadowMountApi,
    WritableStorage,
    Networking,
    Database,
    OverlayCapability,
};

struct CheckResult {
    CheckId id = CheckId::Firmware;
    std::string label;      // "ShadowMount API"
    CheckStatus status = CheckStatus::Skipped;
    std::string summary;    // "connected (ShadowMount+ 1.7, API v1)"
    std::string detail;     // technical cause for the log, may be empty
};

enum class FeatureState { Available, Disabled, NotImplemented };
std::string_view toString(FeatureState state) noexcept;

struct Feature {
    std::string name;
    FeatureState state = FeatureState::Disabled;
    std::string reason;     // why it is not available (empty when available)
};

struct FeatureAvailability {
    Feature gameLibrary{"Game library", FeatureState::Disabled, {}};
    Feature modBrowsing{"Mod browsing", FeatureState::Disabled, {}};
    Feature downloading{"Downloading", FeatureState::Disabled, {}};
    Feature installation{"Installation", FeatureState::Disabled, {}};

    // Safe Mode = installation is not available, for whatever reason.
    bool safeMode() const noexcept { return installation.state != FeatureState::Available; }
    std::vector<const Feature*> all() const { return {&gameLibrary, &modBrowsing, &downloading, &installation}; }
};

// What this build implements. Kept separate from detection so the report never claims a
// feature that does not exist yet.
struct BuildFeatures {
    bool modBrowsing = false;   // Phase 2
    bool downloading = false;   // Phase 3
    bool installation = false;  // Phase 5
};

FeatureAvailability computeFeatures(const std::vector<CheckResult>& checks, const BuildFeatures& build);

struct SystemReport {
    std::string generatedAt;
    std::string platformName;
    std::vector<CheckResult> checks;
    FeatureAvailability features;
    // Whether hard links work in Akeno's storage (decides links or copies for overlays).
    std::optional<bool> hardLinksSupported;

    const CheckResult* find(CheckId id) const;
    std::string toText() const;
};

struct SystemCheckDependencies {
    platform::IPlatform* platform = nullptr;
    games::IGameDiscoveryProvider* gameProvider = nullptr;  // null if it could not be created
    std::optional<Error> gameProviderError;                 // why it is null
    network::IHttpClient* http = nullptr;
    const security::SafeFs* fs = nullptr;
    AppPaths paths;
    database::Database* db = nullptr;                       // null if it could not be opened
    std::optional<Error> databaseError;
    std::string networkProbeUrl;
    BuildFeatures build;
};

class SystemChecker {
public:
    explicit SystemChecker(SystemCheckDependencies deps) : deps_(std::move(deps)) {}

    // Runs every check in order. `onResult` (optional) is called after each check, from the
    // calling thread, so a UI can show progress.
    SystemReport run(const std::function<void(const CheckResult&)>& onResult = {});

    CheckResult checkFirmware();
    CheckResult checkHomebrewEnvironment();
    // Returns the "ShadowMount" and "ShadowMount API" lines.
    std::pair<CheckResult, CheckResult> checkShadowMount();
    CheckResult checkWritableStorage();
    CheckResult checkNetworking();
    CheckResult checkDatabase();
    CheckResult checkOverlayCapability(const CheckResult& shadowMountApi);

private:
    SystemCheckDependencies deps_;
    std::optional<bool> hardLinks_;
};

}  // namespace akeno::app
