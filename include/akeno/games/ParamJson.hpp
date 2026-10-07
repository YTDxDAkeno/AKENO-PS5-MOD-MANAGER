// SPDX-License-Identifier: GPL-3.0-or-later
// Game versions from a PS5 game's own sce_sys/param.json ("contentVersion"), for ShadowMountPlus
// builds that do not report versions (1.7beta4 and older send no "version" field). Only reads,
// with a size limit; the value is used only if it looks like a version number.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "akeno/games/GameInfo.hpp"

namespace akeno::games {

inline constexpr std::size_t kMaxParamJsonBytes = 1024 * 1024;  // ShadowMountPlus MAX_PARAM_JSON_SIZE
inline constexpr std::string_view kDefaultAppmetaBase = "/user/appmeta";

// Digits separated by single dots, 1 to 32 characters ("01.005.000", "1.02").
bool isPlausibleGameVersion(std::string_view text) noexcept;

std::optional<std::string> contentVersionFromParamJson(std::string_view json);

struct VersionLookup {
    std::string version;
    std::filesystem::path source;  // the param.json it came from
};

// Tries, in order: <install path>/sce_sys/param.json (folder games), <runtime path>/sce_sys/
// param.json (mounted games), <appmeta>/<TITLE_ID>/param.json (the copy the system keeps).
std::optional<VersionLookup> readInstalledVersion(const GameInfo& game,
                                                  const std::filesystem::path& appmetaBase = kDefaultAppmetaBase);

}  // namespace akeno::games
