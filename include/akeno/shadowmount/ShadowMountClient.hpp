// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only client for the ShadowMountPlus HTTP/JSON API v1 (docs/shadowmount.md).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/network/Http.hpp"

namespace akeno::shadowmount {

inline constexpr std::uint16_t kDefaultPort = 10101;
inline constexpr int kSupportedApiVersion = 1;

namespace capability {
inline constexpr std::string_view kListGames = "list_games";
inline constexpr std::string_view kGameIcon = "game_icon";
inline constexpr std::string_view kStorageSpace = "storage_space";
inline constexpr std::string_view kManageSettings = "manage_settings";
}  // namespace capability

struct VersionInfo {
    int apiVersion = 0;
    std::string shadowMountVersion;
    std::set<std::string, std::less<>> capabilities;

    bool has(std::string_view capability) const { return capabilities.find(capability) != capabilities.end(); }
};

struct Game {
    std::string path;
    std::string runtimePath;
    std::string sourceType;   // folder | image | pkg
    std::string imageType;
    std::string platform;     // ps5 | ps4 | unknown
    std::string titleId;      // validated
    std::string contentId;
    std::string titleName;
    std::string version;
    std::string lastAccessTime;
    std::string iconUrl;      // informational only; Akeno builds icon URLs itself
    std::uint64_t appDbSizeBytes = 0;
    bool installed = false;
    bool managed = false;
    bool mounted = false;
    bool imageBacked = false;
    bool sourceAvailable = false;
    bool installedPkg = false;
};

struct GameList {
    std::vector<Game> games;
    std::vector<std::string> skipped;  // reasons for entries that were ignored
};

struct StorageMount {
    std::string source;
    std::string mountPoint;
    std::string filesystem;
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
    bool readOnly = false;
};

// Response parsers (public for unit tests). Each validates `status == 0` and field types.
Result<VersionInfo> parseVersionResponse(std::string_view body);
Result<GameList> parseGamesResponse(std::string_view body);
Result<std::vector<StorageMount>> parseStorageResponse(std::string_view body);
Result<std::vector<std::string>> parseSettingsScanPaths(std::string_view body);

struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = kDefaultPort;
};

class ShadowMountClient {
public:
    // Fails if the endpoint is not a loopback address: the API has no authentication and must
    // never be reached over the network.
    static Result<ShadowMountClient> create(network::IHttpClient& http, Endpoint endpoint);

    Result<VersionInfo> version();
    Result<GameList> games();
    Result<std::vector<StorageMount>> storage();
    Result<std::vector<std::string>> customScanPaths();
    Result<json::Json> diagnosticSettings(); // strict, allowlisted read-only settings snapshot
    // PNG bytes of a game's icon. `titleId` must be valid.
    Result<std::string> icon(std::string_view titleId, bool thumbnail);

    const Endpoint& endpoint() const noexcept { return endpoint_; }
    std::string baseUrl() const;

private:
    ShadowMountClient(network::IHttpClient& http, Endpoint endpoint) : http_(&http), endpoint_(std::move(endpoint)) {}
    Result<std::string> postJson(std::string_view route, std::string_view body, std::size_t maxBytes,
                                 long timeoutMs);

    network::IHttpClient* http_;
    Endpoint endpoint_;
};

}  // namespace akeno::shadowmount
