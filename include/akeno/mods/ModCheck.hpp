// SPDX-License-Identifier: GPL-3.0-or-later
// The check of a downloaded mod: unpack it into a fresh staging folder (journaled, so an
// interruption can be cleaned up), prepare it for the installed game (layout, content, Unreal
// containers, compatibility; see ModPreparation.hpp), predict conflicts, describe the install
// plan, then delete the staging folder again. Only the report is kept, in cache/analysis/.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/compatibility/CompatibilityEngine.hpp"
#include "akeno/core/AppPaths.hpp"
#include "akeno/core/OperationJournal.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/mods/ArchiveLayout.hpp"
#include "akeno/mods/ModAnalyzer.hpp"
#include "akeno/mods/ModPreparation.hpp"
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
    // The installed game and the mod's origin (see PreparationRequest).
    bool curated = false;             // archiveRoot/targetPrefix come from a curated manifest
    bool pcSource = false;
    std::string gameVersion;
    std::string contentId;
    bool installedPkg = false;
    std::string gameFolder;           // physical folder of a folder game; empty otherwise
    std::vector<InstalledModPaths> installedMods;
    const compatibility::Registry* registry = nullptr;
};

// What the report keeps about the mod's Unreal containers.
struct UnrealSetReport {
    std::string name;                 // folder/stem
    std::string kind;
    std::vector<std::string> members;
    bool companionsVerified = false;
    int tocVersion = 0;
    int pakVersion = 0;
    std::string containerId;          // hex
    std::string containerFlags;
    std::vector<std::string> packages;
    std::vector<std::string> issues;
    std::vector<std::string> unknowns;
    std::vector<std::string> evidence;
};

struct UnrealReport {
    bool detected = false;
    std::string format;
    std::vector<UnrealSetReport> sets;
    std::vector<std::string> loaders;
    std::vector<std::string> modFolders;
    std::vector<std::string> containedPackages;
    std::vector<std::string> importedPackages;  // first 200
    std::size_t importedTotal = 0;
    std::size_t looseAssets = 0;
    std::size_t configFiles = 0;
    int maxTocVersion = 0;
    int maxPakVersion = 0;
    bool unversioned = false;
    bool compatibilityUnknown = false;
};

// What the report keeps about the installed game (read-only observations at check time).
struct GameReport {
    std::string version;
    std::string contentId;
    std::string region;
    bool listed = false;
    bool complete = false;
    std::string reason;
    std::size_t entries = 0;
    std::vector<std::string> topLevel;
    std::string paksDirectory;
    std::vector<std::string> containers;   // "path: IoStore v8, flags ..." (first 40)
    int maxTocVersion = 0;
    int maxPakVersion = 0;
    bool signedContainers = false;
    bool encryptedContainers = false;
    bool packageIdsComplete = false;
    std::size_t packageIds = 0;
};

struct ModCheckReport {
    static constexpr int kSchemaVersion = 2;
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
    ArchiveLayout layout;        // stored without `mapping`, which the files' install paths repeat
    UnrealReport unreal;
    GameReport game;
    compatibility::Assessment assessment;
    // Not stored; recomputed when a report is shown.
    std::vector<Conflict> conflicts;           // with other checked mods of the game
    std::vector<Conflict> installedConflicts;  // with mods installed for the game
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

UnrealReport summarizeUnreal(const unreal::UnrealAnalysis& analysis);
GameReport summarizeGame(const Preparation& preparation, const std::string& version, const std::string& contentId);

// Reports in cache/analysis/<download id>.json.
std::filesystem::path reportPath(const AppPaths& paths, const std::string& downloadId);
Status saveReport(const security::SafeFs& fs, const AppPaths& paths, const ModCheckReport& report);
// Reads and validates a stored report; anything unexpected (including an older schema) makes it
// invalid, and the caller checks the download again.
Result<ModCheckReport> loadReport(const AppPaths& paths, const std::string& downloadId);
// All valid stored reports for a game (for conflicts), except `excludeId`.
std::vector<ModCheckReport> loadReportsForTitle(const AppPaths& paths, const std::string& titleId,
                                                const std::string& excludeId);
// Adds conflicts with the other checked mods and the installed mods of the same game, and the
// install plan.
void completeReport(ModCheckReport& report, const AppPaths& paths, std::optional<bool> hardLinks = std::nullopt,
                    const std::vector<InstalledModPaths>& installed = {});

std::string_view toString(providers::CompatibilityStatus status) noexcept;
std::optional<providers::CompatibilityStatus> parseCompatibilityStatus(std::string_view text) noexcept;

}  // namespace akeno::mods
