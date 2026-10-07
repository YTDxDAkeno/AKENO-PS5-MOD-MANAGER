// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/providers/GameBananaProvider.hpp"

#include <algorithm>

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/mods/Catalog.hpp"
#include "akeno/network/Url.hpp"

namespace akeno::providers {

namespace {

constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;
constexpr std::size_t kPerPage = 30;

bool isNumber(std::string_view text) {
    return !text.empty() && text.size() <= 12 &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string idText(const json::Json& object, std::string_view key) {
    if (auto number = json::getInt(object, key); number && *number > 0) return std::to_string(*number);
    return {};
}

// GameBanana texts are HTML; only plain text is shown.
std::string plainText(std::string_view text, std::size_t maxBytes) {
    std::string out;
    bool inTag = false;
    for (char c : text) {
        if (c == '<') {
            inTag = true;
            out += ' ';
        } else if (c == '>') {
            inTag = false;
        } else if (!inTag) {
            out += c;
        }
    }
    for (auto [entity, plain] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}}) {
        std::size_t at = 0;
        while ((at = out.find(entity, at)) != std::string::npos) {
            out.replace(at, entity.size(), plain);
            at += plain.size();
        }
    }
    std::string collapsed;
    for (char c : out) {
        if (c == ' ' && (collapsed.empty() || collapsed.back() == ' ')) continue;
        collapsed += c;
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    return strings::sanitizeForDisplay(collapsed, maxBytes);
}

std::string httpsOrEmpty(const std::string& url) { return mods::isAllowedRemoteUrl(url) ? url : std::string(); }

// _aPreviewMedia._aImages[0]: _sBaseUrl + "/" + (_sFile220 or _sFile)
std::string previewImage(const json::Json& record, bool small) {
    const json::Json* media = json::getObject(record, "_aPreviewMedia");
    const json::Json* images = media != nullptr ? json::getArray(*media, "_aImages") : nullptr;
    if (images == nullptr || images->empty() || !(*images)[0].is_object()) return {};
    const json::Json& image = (*images)[0];
    const std::string base = json::getString(image, "_sBaseUrl").value_or("");
    std::string file = small ? json::getString(image, "_sFile220").value_or("") : std::string();
    if (file.empty()) file = json::getString(image, "_sFile").value_or("");
    if (base.empty() || file.empty() || file.find('/') != std::string::npos) return {};
    return httpsOrEmpty(base + "/" + file);
}

std::optional<ModSummary> summaryFrom(const json::Json& record) {
    if (!record.is_object()) return std::nullopt;
    if (json::getString(record, "_sModelName").value_or("Mod") != "Mod") return std::nullopt;
    if (json::getBool(record, "_bIsNsfw").value_or(false)) return std::nullopt;
    const std::string id = idText(record, "_idRow");
    if (id.empty()) return std::nullopt;
    ModSummary summary;
    summary.ref = {std::string(kGameBananaProviderId), id};
    summary.name = plainText(json::getString(record, "_sName").value_or(""), limits::kMaxDisplayStringBytes);
    if (summary.name.empty()) return std::nullopt;
    if (const json::Json* submitter = json::getObject(record, "_aSubmitter")) {
        summary.author = plainText(json::getString(*submitter, "_sName").value_or(""), 128);
    }
    summary.version = plainText(json::getString(record, "_sVersion").value_or(""), 64);
    summary.thumbnailUrl = previewImage(record, true);
    summary.compatibility = CompatibilityStatus::Experimental;
    summary.categories = {"GameBanana"};
    if (auto likes = json::getInt(record, "_nLikeCount"); likes && *likes > 0) {
        summary.categories.push_back(strings::concat(*likes, " likes"));
    }
    return summary;
}

std::string formatOf(std::string_view fileName) {
    const std::string name = strings::toLowerAscii(fileName);
    if (strings::endsWith(name, ".zip")) return "zip";
    if (strings::endsWith(name, ".7z")) return "7z";
    return {};
}

}  // namespace

GameBananaProvider::GameBananaProvider(network::IHttpClient& http, std::string baseUrl)
    : http_(http), baseUrl_(std::move(baseUrl)) {}

void GameBananaProvider::setInstalledGames(std::vector<InstalledGameName> games) {
    std::lock_guard<std::mutex> lock(mutex_);
    installed_ = std::move(games);
}

Result<std::string> GameBananaProvider::get(const std::string& path, const CancellationToken* cancel) {
    network::HttpRequest request;
    request.url = baseUrl_ + path;
    request.maxResponseBytes = kMaxBytes;
    request.headers = {{"Accept", "application/json"},
                       {"User-Agent", "AkenoPS5ModManager/" + std::string(build::version())}};
    auto response = http_.send(request, cancel);
    if (!response) return std::move(response).error();
    if (response->status == 429) {
        return makeError(ErrorCode::Unavailable, "GameBanana limits how often apps may ask. Try again later.");
    }
    if (!response->isSuccess()) {
        return makeError(ErrorCode::HttpStatus, strings::concat("GameBanana answered HTTP ", response->status, "."),
                         path);
    }
    return std::move(response->body);
}

Result<std::vector<ProviderGame>> GameBananaProvider::listGames(const CancellationToken* cancel) {
    std::vector<InstalledGameName> installed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        installed = installed_;
    }
    std::vector<ProviderGame> games;
    std::map<std::string, std::string> titles;
    std::optional<Error> firstError;
    for (const auto& mine : installed) {
        if (cancel != nullptr && cancel->cancelled()) break;
        const std::string wanted = normalizeGameName(mine.name);
        if (wanted.empty()) continue;
        auto body = get("/apiv11/Util/Game/NameMatch?_sName=" + network::percentEncode(mine.name), cancel);
        if (!body) {
            if (!firstError) firstError = body.error();
            continue;
        }
        auto parsed = json::parseBounded(body.value(), kMaxBytes);
        const json::Json* records = parsed && parsed->is_object() ? json::getArray(parsed.value(), "_aRecords") : nullptr;
        if (records == nullptr) continue;
        for (const auto& record : *records) {
            if (!record.is_object()) continue;
            const std::string name = json::getString(record, "_sName").value_or("");
            const std::string id = idText(record, "_idRow");
            if (id.empty() || normalizeGameName(name) != wanted) continue;
            ProviderGame entry;
            entry.providerGameId = std::string(kGameBananaGamePrefix) + id;
            entry.name = plainText(name, limits::kMaxDisplayStringBytes) + " (GameBanana)";
            entry.titleIds = {mine.titleId};
            entry.modCount = static_cast<int>(json::getInt(record, "_nModCount").value_or(0));
            titles[id] = mine.titleId;
            games.push_back(std::move(entry));
            break;
        }
    }
    if (games.empty() && firstError) return *firstError;
    std::lock_guard<std::mutex> lock(mutex_);
    titleIdByGame_ = std::move(titles);
    return games;
}

Result<ModPage> GameBananaProvider::list(const SearchQuery& query, const CancellationToken* cancel) {
    if (!strings::startsWith(query.providerGameId, kGameBananaGamePrefix)) {
        return makeError(ErrorCode::InvalidArgument, "Not a GameBanana game.");
    }
    const std::string gameId = query.providerGameId.substr(kGameBananaGamePrefix.size());
    if (!isNumber(gameId)) return makeError(ErrorCode::InvalidArgument, "Not a GameBanana game.");
    const std::size_t page = query.offset / kPerPage + 1;
    std::string path = strings::concat("/apiv11/Mod/Index?_nPerpage=", kPerPage, "&_nPage=", page,
                                       "&_aFilters%5BGeneric_Game%5D=", gameId);
    if (query.order == BrowseOrder::Newest) path += "&_sSort=new";
    if (query.order == BrowseOrder::Popular) path += "&_sSort=popular";
    if (!query.text.empty()) path += "&_aFilters%5BGeneric_Name%5D=" + network::percentEncode(query.text);
    auto body = get(path, cancel);
    if (!body) return std::move(body).error();
    auto parsed = json::parseBounded(body.value(), kMaxBytes);
    const json::Json* records = parsed && parsed->is_object() ? json::getArray(parsed.value(), "_aRecords") : nullptr;
    if (records == nullptr) return makeError(ErrorCode::ParseError, "GameBanana sent an unexpected mod list.");
    ModPage result;
    for (const auto& record : *records) {
        if (auto summary = summaryFrom(record)) result.mods.push_back(std::move(summary).value());
        if (result.mods.size() >= query.limit) break;
    }
    result.total = result.mods.size() + query.offset;
    if (const json::Json* meta = json::getObject(parsed.value(), "_aMetadata")) {
        if (auto count = json::getInt(*meta, "_nRecordCount"); count && *count >= 0) {
            result.total = static_cast<std::size_t>(*count);
        }
    }
    return result;
}

Result<ModPage> GameBananaProvider::browseMods(const SearchQuery& query, const CancellationToken* cancel) {
    SearchQuery browse = query;
    browse.text.clear();
    return list(browse, cancel);
}

Result<ModPage> GameBananaProvider::searchMods(const SearchQuery& query, const CancellationToken* cancel) {
    return list(query, cancel);
}

Result<ModDetails> GameBananaProvider::getModDetails(const ModRef& ref, const std::optional<GameContext>& game,
                                                     const CancellationToken* cancel) {
    if (ref.providerId != kGameBananaProviderId || !isNumber(ref.modId)) {
        return makeError(ErrorCode::InvalidArgument, "Not a GameBanana mod.", ref.modId);
    }
    auto body = get("/apiv11/Mod/" + ref.modId + "/ProfilePage", cancel);
    if (!body) return std::move(body).error();
    auto parsed = json::parseBounded(body.value(), kMaxBytes);
    if (!parsed || !parsed->is_object()) return makeError(ErrorCode::ParseError, "GameBanana sent an unexpected answer.");
    json::Json record = parsed.value();
    if (!record.contains("_idRow")) record["_idRow"] = std::stoll(ref.modId);
    auto summary = summaryFrom(record);
    if (!summary) return makeError(ErrorCode::NotFound, "This mod is not available on GameBanana.");
    ModDetails details;
    details.summary = std::move(summary).value();
    details.description = plainText(json::getString(record, "_sText").value_or(""), 6000);
    details.homepage = httpsOrEmpty(json::getString(record, "_sProfileUrl").value_or(""));
    if (details.homepage.empty()) details.homepage = "https://gamebanana.com/mods/" + ref.modId;
    details.license = "See the mod page on GameBanana";
    if (const json::Json* gameInfo = json::getObject(record, "_aGame")) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = titleIdByGame_.find(idText(*gameInfo, "_idRow")); it != titleIdByGame_.end()) {
            details.titleIds = {it->second};
        }
    }
    if (game && details.titleIds.empty()) details.titleIds = {game->titleId};
    if (auto image = previewImage(record, false); !image.empty()) details.screenshots.push_back({image, ""});

    if (const json::Json* files = json::getArray(record, "_aFiles")) {
        for (const auto& item : *files) {
            if (!item.is_object()) continue;
            const std::string fileId = idText(item, "_idRow");
            const auto size = json::getInt(item, "_nFilesize");
            const std::string name = json::getString(item, "_sFile").value_or("");
            const std::string format = formatOf(name);
            if (fileId.empty() || !size || *size <= 0 || format.empty()) continue;  // rar etc. are not supported
            ModFile file;
            file.fileId = fileId;
            file.displayName = plainText(name, 200);
            file.version = details.summary.version;
            file.sizeBytes = static_cast<std::uint64_t>(*size);
            file.format = format;
            details.files.push_back(std::move(file));
        }
    }
    if (!details.files.empty()) details.files.front().primary = true;

    details.compatibilityReasons = {
        "GameBanana hosts mods for the PC version. This one is not tested on PS5.",
        "After the download Akeno checks every file: Windows programs, script loaders and console code are refused.",
    };
    details.checks = {
        {"Source", "GameBanana (PC mod)", CompatibilityCheck::Mark::Warn},
        {"Game", "matched by name", CompatibilityCheck::Mark::Warn},
        {"Files", details.files.empty() ? "no zip or 7z file" : strings::concat(details.files.size(), " zip/7z file(s)"),
         details.files.empty() ? CompatibilityCheck::Mark::Fail : CompatibilityCheck::Mark::Pass},
        {"PS5 test", "none", CompatibilityCheck::Mark::Unknown},
    };
    details.risk = "HIGH";
    details.installable = !details.files.empty();
    details.needsConfirmation = true;
    return details;
}

Result<std::vector<ModFile>> GameBananaProvider::getFiles(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->files;
}

Result<std::vector<Screenshot>> GameBananaProvider::getScreenshots(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->screenshots;
}

Result<std::vector<ModDependency>> GameBananaProvider::getDependencies(const ModRef&, const CancellationToken*) {
    return std::vector<ModDependency>{};
}

Result<DownloadTicket> GameBananaProvider::resolveDownload(const ModRef& ref, const ModFile& file,
                                                           const CancellationToken*) {
    if (ref.providerId != kGameBananaProviderId || !isNumber(file.fileId)) {
        return makeError(ErrorCode::InvalidArgument, "Not a GameBanana file.");
    }
    // GameBanana's download address redirects to its file server; downloads are free.
    DownloadTicket ticket;
    ticket.url = "https://gamebanana.com/dl/" + file.fileId;
    ticket.expectedSize = file.sizeBytes;
    ticket.format = file.format;  // no SHA-256 published: the size is checked, the hash recorded
    return ticket;
}

Result<std::string> GameBananaProvider::getLatestVersion(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->summary.version;
}

}  // namespace akeno::providers
