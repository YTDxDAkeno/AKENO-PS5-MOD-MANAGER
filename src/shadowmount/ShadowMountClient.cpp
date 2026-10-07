// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/shadowmount/ShadowMountClient.hpp"

#include "akeno/core/Json.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/network/Url.hpp"

namespace akeno::shadowmount {

namespace {

constexpr long kApiTimeoutMs = 8000;
constexpr long kGamesTimeoutMs = 30000;  // large libraries can take a moment to serialize

Result<json::Json> parseEnvelope(std::string_view body, std::size_t maxBytes) {
    auto document = json::parseBounded(body, maxBytes);
    if (!document) {
        return makeError(ErrorCode::ParseError, "ShadowMountPlus sent a response Akeno could not read.",
                         document.error().describe());
    }
    if (!document->is_object()) {
        return makeError(ErrorCode::SchemaError, "ShadowMountPlus sent an unexpected response.",
                         "top-level value is not an object");
    }
    auto status = json::getInt(document.value(), "status");
    if (!status) {
        return makeError(ErrorCode::SchemaError, "ShadowMountPlus sent an unexpected response.",
                         "missing numeric 'status'");
    }
    if (*status != 0) {
        std::string text = json::displayString(document.value(), "error", "unknown error", 300);
        std::string reason = json::displayString(document.value(), "error_reason", "", 64);
        return makeError(*status == 16 /*EBUSY*/ ? ErrorCode::Busy : ErrorCode::Unavailable,
                         "ShadowMountPlus reported an error: " + text,
                         strings::concat("status ", *status, reason.empty() ? "" : " reason " + reason));
    }
    return document;
}

std::uint64_t nonNegative(const json::Json& object, std::string_view key) {
    auto value = json::getInt(object, key);
    return (value && *value > 0) ? static_cast<std::uint64_t>(*value) : 0;
}

}  // namespace

Result<VersionInfo> parseVersionResponse(std::string_view body) {
    auto document = parseEnvelope(body, limits::kMaxJsonSmallResponse);
    if (!document) {
        return std::move(document).error();
    }
    const json::Json& doc = document.value();
    VersionInfo info;
    auto api = json::getInt(doc, "api_version");
    if (!api) {
        return makeError(ErrorCode::SchemaError, "ShadowMountPlus did not report its API version.");
    }
    info.apiVersion = static_cast<int>(*api);
    info.shadowMountVersion = json::displayString(doc, "shadowmount_version", "unknown", 64);
    if (const auto* caps = json::getArray(doc, "capabilities")) {
        for (const auto& cap : *caps) {
            if (cap.is_string() && info.capabilities.size() < 128) {
                info.capabilities.insert(strings::sanitizeForDisplay(cap.get<std::string>(), 64));
            }
        }
    }
    return info;
}

Result<GameList> parseGamesResponse(std::string_view body) {
    auto document = parseEnvelope(body, limits::kMaxJsonGameList);
    if (!document) {
        return std::move(document).error();
    }
    const json::Json* games = json::getArray(document.value(), "games");
    if (games == nullptr) {
        return makeError(ErrorCode::SchemaError, "ShadowMountPlus did not return a game list.",
                         "missing 'games' array");
    }
    GameList list;
    for (const auto& item : *games) {
        if (list.games.size() >= limits::kMaxGames) {
            list.skipped.push_back(strings::concat("more than ", limits::kMaxGames, " games; the rest were ignored"));
            break;
        }
        if (!item.is_object()) {
            list.skipped.push_back("entry is not an object");
            continue;
        }
        auto titleId = json::getString(item, "title_id");
        if (!titleId || !games::isValidTitleId(*titleId)) {
            list.skipped.push_back("invalid title_id: " +
                                   strings::sanitizeForDisplay(titleId.value_or("<missing>"), 40));
            continue;
        }
        Game game;
        game.titleId = *titleId;
        game.titleName = json::displayString(item, "title_name", game.titleId);
        game.version = json::displayString(item, "version", "", 32);
        game.contentId = json::displayString(item, "content_id", "", 128);
        game.platform = json::displayString(item, "platform", "unknown", 16);
        game.sourceType = json::displayString(item, "source_type", "", 16);
        game.imageType = json::displayString(item, "image_type", "", 16);
        game.path = json::displayString(item, "path", "", limits::kMaxPathBytes);
        game.runtimePath = json::displayString(item, "runtime_path", "", limits::kMaxPathBytes);
        game.lastAccessTime = json::displayString(item, "last_access_time", "", 64);
        game.iconUrl = json::displayString(item, "icon_url", "", 256);
        game.appDbSizeBytes = nonNegative(item, "app_db_size_bytes");
        game.installed = json::getBool(item, "installed").value_or(false);
        game.managed = json::getBool(item, "managed").value_or(false);
        game.mounted = json::getBool(item, "mounted").value_or(false);
        game.imageBacked = json::getBool(item, "image_backed").value_or(false);
        game.sourceAvailable = json::getBool(item, "source_available").value_or(true);
        game.installedPkg = json::getBool(item, "installed_pkg").value_or(false);
        list.games.push_back(std::move(game));
    }
    return list;
}

Result<std::vector<StorageMount>> parseStorageResponse(std::string_view body) {
    auto document = parseEnvelope(body, limits::kMaxJsonSmallResponse);
    if (!document) {
        return std::move(document).error();
    }
    const json::Json* mounts = json::getArray(document.value(), "mounts");
    if (mounts == nullptr) {
        return makeError(ErrorCode::SchemaError, "ShadowMountPlus did not return storage information.");
    }
    std::vector<StorageMount> result;
    for (const auto& item : *mounts) {
        if (!item.is_object() || result.size() >= 64) continue;
        StorageMount mount;
        mount.source = json::displayString(item, "source", "", 256);
        mount.mountPoint = json::displayString(item, "mount_point", "", limits::kMaxPathBytes);
        mount.filesystem = json::displayString(item, "filesystem", "", 32);
        mount.totalBytes = nonNegative(item, "total_bytes");
        mount.availableBytes = nonNegative(item, "available_bytes");
        mount.readOnly = json::getBool(item, "read_only").value_or(true);
        result.push_back(std::move(mount));
    }
    return result;
}

Result<std::vector<std::string>> parseSettingsScanPaths(std::string_view body) {
    auto document = parseEnvelope(body, limits::kMaxJsonSmallResponse);
    if (!document) {
        return std::move(document).error();
    }
    std::vector<std::string> paths;
    if (const json::Json* scanPaths = json::getArray(document.value(), "scan_paths")) {
        for (const auto& item : *scanPaths) {
            if (item.is_string() && paths.size() < 256) {
                const auto& text = item.get_ref<const std::string&>();
                if (!text.empty() && text.front() == '/' && text.size() <= limits::kMaxPathBytes) {
                    paths.push_back(text);
                }
            }
        }
    }
    return paths;
}

Result<ShadowMountClient> ShadowMountClient::create(network::IHttpClient& http, Endpoint endpoint) {
    if (!network::isLoopbackHost(endpoint.host)) {
        return makeError(ErrorCode::SafetyViolation,
                         "ShadowMountPlus must be reached on this console only (127.0.0.1).", endpoint.host);
    }
    if (endpoint.port == 0) {
        return makeError(ErrorCode::InvalidArgument, "The ShadowMountPlus port is not valid.");
    }
    return ShadowMountClient(http, std::move(endpoint));
}

std::string ShadowMountClient::baseUrl() const {
    std::string host = endpoint_.host.find(':') != std::string::npos ? "[" + endpoint_.host + "]" : endpoint_.host;
    return strings::concat("http://", host, ":", endpoint_.port);
}

Result<std::string> ShadowMountClient::postJson(std::string_view route, std::string_view body, std::size_t maxBytes,
                                                long timeoutMs) {
    network::HttpRequest request;
    request.method = network::HttpMethod::Post;
    request.url = baseUrl() + std::string(route);
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.body = std::string(body);
    request.maxResponseBytes = maxBytes;
    request.connectTimeoutMs = 3000;
    request.totalTimeoutMs = timeoutMs;
    request.maxRedirects = 0;
    auto response = http_->send(request);
    if (!response) {
        Error error = std::move(response).error();
        if (error.code == ErrorCode::Unavailable || error.code == ErrorCode::Timeout) {
            error.message = "ShadowMountPlus is not answering on port " + std::to_string(endpoint_.port) +
                            ". Make sure the ShadowMountPlus payload is running.";
        }
        return error;
    }
    if (response->status == 404 && route == "/api/v1/version") {
        return makeError(ErrorCode::Unsupported,
                         "This ShadowMountPlus version has no API. Version 1.7 or newer is required.",
                         "HTTP 404 for /api/v1/version");
    }
    // Error statuses still carry the JSON envelope with ShadowMountPlus' explanation.
    if (!response->isSuccess() && response->body.empty()) {
        return makeError(ErrorCode::HttpStatus, "ShadowMountPlus rejected the request.",
                         strings::concat("HTTP ", response->status, " for ", route));
    }
    return std::move(response->body);
}

Result<VersionInfo> ShadowMountClient::version() {
    auto body = postJson("/api/v1/version", "{}", limits::kMaxJsonSmallResponse, kApiTimeoutMs);
    if (!body) {
        return std::move(body).error();
    }
    return parseVersionResponse(body.value());
}

Result<GameList> ShadowMountClient::games() {
    auto body = postJson("/api/v1/games", R"({"include_size":false})", limits::kMaxJsonGameList, kGamesTimeoutMs);
    if (!body) {
        return std::move(body).error();
    }
    return parseGamesResponse(body.value());
}

Result<std::vector<StorageMount>> ShadowMountClient::storage() {
    auto body = postJson("/api/v1/storage", "{}", limits::kMaxJsonSmallResponse, kApiTimeoutMs);
    if (!body) {
        return std::move(body).error();
    }
    return parseStorageResponse(body.value());
}

Result<std::vector<std::string>> ShadowMountClient::customScanPaths() {
    auto body = postJson("/api/v1/settings", "{}", limits::kMaxJsonSmallResponse, kApiTimeoutMs);
    if (!body) {
        return std::move(body).error();
    }
    return parseSettingsScanPaths(body.value());
}

Result<std::string> ShadowMountClient::icon(std::string_view titleId, bool thumbnail) {
    if (!games::isValidTitleId(titleId)) {
        return makeError(ErrorCode::InvalidArgument, "Invalid title ID.", std::string(titleId));
    }
    network::HttpRequest request;
    request.method = network::HttpMethod::Get;
    request.url = baseUrl() + "/api/v1/games/icon?title_id=" + std::string(titleId) + (thumbnail ? "&size=thumb" : "");
    request.maxResponseBytes = limits::kMaxImageBytes;
    request.connectTimeoutMs = 3000;
    request.totalTimeoutMs = kApiTimeoutMs;
    request.maxRedirects = 0;
    auto response = http_->send(request);
    if (!response) {
        return std::move(response).error();
    }
    if (response->status == 404) {
        return makeError(ErrorCode::NotFound, "This game has no icon.", std::string(titleId));
    }
    if (!response->isSuccess()) {
        return makeError(ErrorCode::HttpStatus, "The game icon could not be loaded.",
                         strings::concat("HTTP ", response->status));
    }
    return std::move(response->body);
}

}  // namespace akeno::shadowmount
