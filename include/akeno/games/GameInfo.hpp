// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace akeno::games {

enum class Platform { Ps5, Ps4, Unknown };
enum class SourceType { Folder, Image, Pkg, Unknown };
enum class TitleKind { Ps5Game, Ps4Game, Homebrew, Other };

std::string_view toString(Platform platform) noexcept;
std::string_view toString(SourceType source) noexcept;
std::string_view displayName(SourceType source) noexcept;  // "Folder", "Disk image", "Installed package"

// Exactly four upper-case letters followed by five digits ("PPSA01234").
bool isValidTitleId(std::string_view titleId) noexcept;
TitleKind classifyTitleId(std::string_view titleId) noexcept;

struct GameInfo {
    std::string titleId;
    std::string name;
    std::string version;        // may be empty when the game does not report one
    std::string contentId;
    Platform platform = Platform::Unknown;
    SourceType sourceType = SourceType::Unknown;
    std::string installPath;
    std::string runtimePath;
    bool mounted = false;
    bool installedPkg = false;
    bool sourceAvailable = true;
    bool hasIcon = false;
    std::string lastAccessTime;  // as reported by the system; used for "recently played" sorting
    std::optional<std::uint64_t> sizeBytes;

    // Filled by later phases. In 0.1.0 installedMods is always 0 (no installer exists yet) and
    // compatibleMods is unknown.
    int installedMods = 0;
    std::optional<int> compatibleMods;
    bool updateAvailable = false;

    // Set by GameLibrary when the version differs from the previous scan.
    std::optional<std::string> previousVersion;

    std::string displayVersion() const { return version.empty() ? "unknown" : version; }
};

}  // namespace akeno::games
