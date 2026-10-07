// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/shadowmount/ShadowMountGameProvider.hpp"

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"

namespace akeno::shadowmount {

games::GameInfo toGameInfo(const Game& game) {
    games::GameInfo info;
    info.titleId = game.titleId;
    info.name = game.titleName.empty() ? game.titleId : game.titleName;
    info.version = game.version;
    if (!info.version.empty()) info.versionSource = "ShadowMountPlus";
    info.contentId = game.contentId;
    if (game.platform == "ps5") {
        info.platform = games::Platform::Ps5;
    } else if (game.platform == "ps4") {
        info.platform = games::Platform::Ps4;
    } else {
        info.platform = games::Platform::Unknown;
    }
    if (game.sourceType == "folder") {
        info.sourceType = games::SourceType::Folder;
    } else if (game.sourceType == "image") {
        info.sourceType = games::SourceType::Image;
    } else if (game.sourceType == "pkg") {
        info.sourceType = games::SourceType::Pkg;
    }
    info.installPath = game.path;
    info.runtimePath = game.runtimePath;
    info.mounted = game.mounted;
    info.installedPkg = game.installedPkg;
    info.sourceAvailable = game.sourceAvailable;
    info.hasIcon = !game.iconUrl.empty();
    info.lastAccessTime = game.lastAccessTime;
    if (game.appDbSizeBytes > 0) {
        info.sizeBytes = game.appDbSizeBytes;
    }
    return info;
}

Result<VersionInfo> ShadowMountGameProvider::ensureVersion() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (version_) {
        return *version_;
    }
    auto version = client_.version();
    if (!version) {
        return std::move(version).error();
    }
    version_ = version.value();
    return version;
}

Result<games::DiscoveryStatus> ShadowMountGameProvider::probe() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version_.reset();  // a probe always asks again
    }
    auto version = ensureVersion();
    if (!version) {
        return std::move(version).error();
    }
    games::DiscoveryStatus status;
    status.providerName = name();
    status.providerVersion = version->shadowMountVersion;
    status.supportsIcons = version->has(capability::kGameIcon);
    if (version->apiVersion != kSupportedApiVersion) {
        status.available = false;
        status.summary = strings::concat("API version ", version->apiVersion, " is not supported (expected ",
                                         kSupportedApiVersion, ")");
        return status;
    }
    if (!version->has(capability::kListGames)) {
        status.available = false;
        status.summary = "the API does not offer the game list (capability 'list_games' missing)";
        return status;
    }
    status.available = true;
    status.summary = strings::concat("connected (ShadowMount+ ", version->shadowMountVersion, ", API v",
                                     version->apiVersion, ")");
    return status;
}

Result<std::vector<games::GameInfo>> ShadowMountGameProvider::discoverGames() {
    auto version = ensureVersion();
    if (!version) {
        return std::move(version).error();
    }
    if (version->apiVersion != kSupportedApiVersion || !version->has(capability::kListGames)) {
        return makeError(ErrorCode::Unsupported,
                         "This ShadowMountPlus version does not provide a compatible game list.",
                         strings::concat("api_version=", version->apiVersion));
    }
    auto list = client_.games();
    if (!list) {
        return std::move(list).error();
    }
    for (const auto& reason : list->skipped) {
        logging::logger().warn("shadowmount", "skipped game entry: " + reason);
    }
    std::vector<games::GameInfo> result;
    result.reserve(list->games.size());
    std::size_t fromParamJson = 0;
    for (const auto& game : list->games) {
        games::GameInfo info = toGameInfo(game);
        // ShadowMountPlus 1.7beta4 and older send no version; read the game's own param.json.
        if (info.version.empty() && info.platform == games::Platform::Ps5) {
            if (auto found = games::readInstalledVersion(info, appmetaBase_)) {
                info.version = found->version;
                info.versionSource = found->source.string();
                ++fromParamJson;
            }
        }
        result.push_back(std::move(info));
    }
    if (fromParamJson > 0) {
        logging::logger().info("shadowmount", strings::concat("read ", fromParamJson,
                                                               " game versions from param.json"));
    }
    return result;
}

Result<std::string> ShadowMountGameProvider::loadIcon(const games::GameInfo& game, games::IconSize size) {
    auto version = ensureVersion();
    if (!version) {
        return std::move(version).error();
    }
    if (!version->has(capability::kGameIcon)) {
        return makeError(ErrorCode::Unsupported, "Game icons are not available from ShadowMountPlus.");
    }
    return client_.icon(game.titleId, size == games::IconSize::Thumbnail);
}

}  // namespace akeno::shadowmount
