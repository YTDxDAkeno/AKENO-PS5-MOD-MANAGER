// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <functional>
#include "akeno/core/AppPaths.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/platform/Platform.hpp"
#include "akeno/security/SafeFs.hpp"
#include "akeno/shadowmount/ShadowMountClient.hpp"

namespace akeno::diagnostics {
struct Limits {
    std::size_t entries = 50000; // shared across all trees in one export
    std::uint64_t hashBytes = 64ull * 1024 * 1024 * 1024;
    std::chrono::seconds duration{300};
};
struct Request {
    AppPaths paths;
    std::filesystem::path backportsRoot = "/data/homebrew/backports";
    std::vector<games::GameInfo> cachedGames;
    std::string titleId; // empty: titles with stored mods (max 32), not every game asset tree
    platform::FirmwareInfo firmware;
    std::vector<std::filesystem::path> logFiles;
    std::filesystem::path smpConfigFile; // optional on-disk config; never treated as live settings
    Limits limits;
};
// Read-only, bounded, no symlinks (including ancestors), no special files or mount operations.
json::Json inventory(const std::filesystem::path& root, const Limits& limits = {},
                     const CancellationToken* cancel = nullptr, bool analyze = true);
// Model the released 1.7beta4 resolver using live API settings and filesystem observations.
// Predictions are not mount/consumption evidence. Missing/ambiguous evidence returns unknown.
json::Json overlaySelection(const games::GameInfo& game, const shadowmount::VersionInfo& version,
                           const json::Json& settings, const std::filesystem::path& akenoBackport);
// Only version/games/settings read routes. Writes a new JSON report under paths.logs()/diagnostics.
// Does not call the installer, recovery, library persistence or any launch/mount API.
Result<std::filesystem::path> exportReport(const Request& request, const security::SafeFs& fs,
    shadowmount::ShadowMountClient& client, const CancellationToken* cancel = nullptr,
    const std::function<void(std::string)>& progress = {});
} // namespace akeno::diagnostics
