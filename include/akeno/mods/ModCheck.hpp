// SPDX-License-Identifier: GPL-3.0-or-later
// The Phase 4 check of a downloaded mod: unpack it into a fresh staging folder (journaled, so
// an interruption can be cleaned up), analyse it, predict conflicts, describe the install plan,
// then delete the staging folder again. Only the report is kept, in cache/analysis/.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/core/AppPaths.hpp"
#include "akeno/core/OperationJournal.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/mods/ModAnalyzer.hpp"
#include "akeno/security/SafeFs.hpp"

namespace akeno::mods {

struct ModCheckRequest {
    std::string downloadId;           // also names the report file
    std::filesystem::path archive;    // the verified download
    ArchiveFormat format = ArchiveFormat::Unknown;
    providers::ModRef mod;
    std::string displayName;
    std::string modVersion;
    std::string titleId;
    std::optional<games::SourceType> sourceType;
    std::string archiveRoot;
    std::string targetPrefix;
    providers::CompatibilityStatus catalogueStatus = providers::CompatibilityStatus::Unknown;
    bool catalogueInstallable = false;
};

struct ModCheckReport {
    static constexpr int kSchemaVersion = 1;
    std::string downloadId;
    providers::ModRef mod;
    std::string displayName;
    std::string modVersion;
    std::string titleId;
    std::string checkedAt;
    std::string archiveFormat;   // as libarchive names it
    std::size_t archiveFiles = 0;
    std::uint64_t unpackedBytes = 0;
    ModAnalysis analysis;
    // Not stored; recomputed when a report is shown.
    std::vector<Conflict> conflicts;
    std::optional<InstallPlan> plan;
};

enum class CheckPhase { Inspecting, Extracting, Analysing, CleaningUp };
std::string_view describe(CheckPhase phase) noexcept;

struct ModCheckEnvironment {
    const security::SafeFs& fs;
    AppPaths paths;
    OperationJournal& journal;
    bool interruptedOperationPending = false;  // never start while recovery is unresolved
    std::uint64_t storageReserve = limits::kStorageSafetyReserveBytes;
    archives::ExtractionLimits limits{};
    std::function<Result<security::StorageSpace>(const std::filesystem::path&)> storageQuery;  // default statvfs
};

// Runs the whole check. `progress` receives the phase and a fraction (0..1) of it.
Result<ModCheckReport> runModCheck(const ModCheckRequest& request, ModCheckEnvironment& environment,
                                   const CancellationToken* cancel = nullptr,
                                   const std::function<void(CheckPhase, double)>& progress = {});

// Reports in cache/analysis/<download id>.json.
std::filesystem::path reportPath(const AppPaths& paths, const std::string& downloadId);
Status saveReport(const security::SafeFs& fs, const AppPaths& paths, const ModCheckReport& report);
// Reads and validates a stored report; anything unexpected makes it invalid.
Result<ModCheckReport> loadReport(const AppPaths& paths, const std::string& downloadId);
// All valid stored reports for a game (for conflicts), except `excludeId`.
std::vector<ModCheckReport> loadReportsForTitle(const AppPaths& paths, const std::string& titleId,
                                                const std::string& excludeId);
// Adds conflicts with the other checked mods of the same game, and the install plan.
void completeReport(ModCheckReport& report, const AppPaths& paths, std::optional<bool> hardLinks = std::nullopt);

std::string_view toString(providers::CompatibilityStatus status) noexcept;
std::optional<providers::CompatibilityStatus> parseCompatibilityStatus(std::string_view text) noexcept;

}  // namespace akeno::mods
