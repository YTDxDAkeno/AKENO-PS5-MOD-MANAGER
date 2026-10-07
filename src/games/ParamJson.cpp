// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/games/ParamJson.hpp"

#include <vector>

#include "akeno/core/Json.hpp"
#include "akeno/security/SafeFs.hpp"

namespace akeno::games {

namespace fs = std::filesystem;

bool isPlausibleGameVersion(std::string_view text) noexcept {
    if (text.empty() || text.size() > 32 || text.front() == '.' || text.back() == '.') return false;
    char previous = '\0';
    for (char c : text) {
        const bool digit = c >= '0' && c <= '9';
        if (!digit && c != '.') return false;
        if (c == '.' && previous == '.') return false;
        previous = c;
    }
    return true;
}

std::optional<std::string> contentVersionFromParamJson(std::string_view json) {
    auto parsed = json::parseBounded(json, kMaxParamJsonBytes);
    if (!parsed || !parsed->is_object()) return std::nullopt;
    auto version = json::getString(parsed.value(), "contentVersion");
    if (!version || !isPlausibleGameVersion(*version)) return std::nullopt;
    return version;
}

namespace {

// Absolute, normalised, without ".." (paths come from ShadowMountPlus and are only read).
std::optional<fs::path> candidate(const std::string& base, std::string_view suffix) {
    if (base.empty() || base.front() != '/' || base.size() > 1024) return std::nullopt;
    fs::path path = fs::path(base) / fs::path(suffix);
    for (const auto& part : path) {
        if (part == "..") return std::nullopt;
    }
    return path.lexically_normal();
}

}  // namespace

std::optional<VersionLookup> readInstalledVersion(const GameInfo& game, const fs::path& appmetaBase) {
    std::vector<fs::path> paths;
    if (game.sourceType == SourceType::Folder) {
        if (auto path = candidate(game.installPath, "sce_sys/param.json")) paths.push_back(*path);
    }
    if (auto path = candidate(game.runtimePath, "sce_sys/param.json")) paths.push_back(*path);
    if (isValidTitleId(game.titleId)) {
        if (auto path = candidate(appmetaBase.string(), game.titleId + "/param.json")) paths.push_back(*path);
    }
    for (const auto& path : paths) {
        auto text = security::readFileBounded(path, kMaxParamJsonBytes);
        if (!text) continue;
        if (auto version = contentVersionFromParamJson(text.value())) {
            return VersionLookup{*version, path};
        }
    }
    return std::nullopt;
}

}  // namespace akeno::games
