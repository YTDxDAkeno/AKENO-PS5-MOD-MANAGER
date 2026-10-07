// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/providers/NexusProvider.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/mods/Catalog.hpp"

namespace akeno::providers {

namespace {

constexpr std::size_t kMaxGamesBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxListBytes = 4 * 1024 * 1024;

bool isDomain(std::string_view text) {
    if (text.empty() || text.size() > 64) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool isNumber(std::string_view text) {
    return !text.empty() && text.size() <= 12 &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "domain/123" -> {domain, 123}
std::optional<std::pair<std::string, std::string>> splitRef(const ModRef& ref) {
    if (ref.providerId != kNexusProviderId) return std::nullopt;
    const auto slash = ref.modId.find('/');
    if (slash == std::string::npos) return std::nullopt;
    std::string domain = ref.modId.substr(0, slash);
    std::string id = ref.modId.substr(slash + 1);
    if (!isDomain(domain) || !isNumber(id)) return std::nullopt;
    return std::make_pair(std::move(domain), std::move(id));
}

std::optional<std::string> domainOf(const SearchQuery& query) {
    if (!strings::startsWith(query.providerGameId, kNexusGamePrefix)) return std::nullopt;
    std::string domain = query.providerGameId.substr(kNexusGamePrefix.size());
    if (!isDomain(domain)) return std::nullopt;
    return domain;
}

// Nexus texts may contain HTML or BBCode; only plain text is shown.
std::string plainText(std::string_view text, std::size_t maxBytes) {
    std::string out;
    bool inTag = false;
    char close = '\0';
    for (char c : text) {
        if (!inTag && (c == '<' || c == '[')) {
            inTag = true;
            close = c == '<' ? '>' : ']';
            continue;
        }
        if (inTag) {
            if (c == close) inTag = false;
            continue;
        }
        out += c;
    }
    for (auto [entity, plain] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"<br />", " "}}) {
        std::size_t at = 0;
        while ((at = out.find(entity, at)) != std::string::npos) {
            out.replace(at, entity.size(), plain);
            at += plain.size();
        }
    }
    return strings::sanitizeForDisplay(out, maxBytes);
}

std::string httpsOrEmpty(std::optional<std::string> url) {
    return (url && mods::isAllowedRemoteUrl(*url)) ? *url : std::string();
}

std::optional<ModSummary> summaryFrom(const json::Json& mod, const std::string& domain) {
    if (!mod.is_object()) return std::nullopt;
    const auto id = json::getInt(mod, "mod_id");
    if (!id || *id <= 0) return std::nullopt;
    if (json::getBool(mod, "contains_adult_content").value_or(false)) return std::nullopt;
    if (!json::getBool(mod, "available").value_or(true)) return std::nullopt;
    if (json::getString(mod, "status").value_or("published") != "published") return std::nullopt;
    ModSummary summary;
    summary.ref = {std::string(kNexusProviderId), domain + "/" + std::to_string(*id)};
    summary.name = plainText(json::getString(mod, "name").value_or(""), limits::kMaxDisplayStringBytes);
    if (summary.name.empty()) return std::nullopt;
    summary.author = plainText(json::getString(mod, "author").value_or(""), 128);
    summary.version = plainText(json::getString(mod, "version").value_or(""), 64);
    summary.shortDescription = plainText(json::getString(mod, "summary").value_or(""), 600);
    summary.thumbnailUrl = httpsOrEmpty(json::getString(mod, "picture_url"));
    summary.compatibility = CompatibilityStatus::Experimental;
    summary.updatedAt = json::getString(mod, "updated_time").value_or("");
    summary.categories = {"Nexus Mods"};
    if (auto endorsements = json::getInt(mod, "endorsement_count")) {
        summary.categories.push_back(strings::concat(*endorsements, " endorsements"));
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

std::string normalizeGameName(std::string_view name) {
    std::string out;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 && u < 0x80) out += static_cast<char>(std::tolower(u));
    }
    return out;
}

bool isPlausibleNexusKey(std::string_view key) {
    if (key.size() < 20 || key.size() > 512) return false;
    return std::all_of(key.begin(), key.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '/' || c == '=' || c == '-' ||
               c == '_';
    });
}

NexusProvider::NexusProvider(network::IHttpClient& http, std::string apiKey, std::string baseUrl)
    : http_(http), apiKey_(std::move(apiKey)), baseUrl_(std::move(baseUrl)) {}

void NexusProvider::setInstalledGames(std::vector<InstalledGameName> games) {
    std::lock_guard<std::mutex> lock(mutex_);
    installed_ = std::move(games);
}

std::optional<bool> NexusProvider::premium() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return premium_;
}

Result<std::string> NexusProvider::get(const std::string& path, std::size_t maxBytes, const CancellationToken* cancel) {
    network::HttpRequest request;
    request.url = baseUrl_ + path;
    request.maxResponseBytes = maxBytes;
    request.headers = {{"apikey", apiKey_},
                       {"Application-Name", "Akeno PS5 Mod Manager"},
                       {"Application-Version", std::string(build::version())},
                       {"Accept", "application/json"}};
    auto response = http_.send(request, cancel);
    if (!response) return std::move(response).error();
    if (response->status == 401) {
        return makeError(ErrorCode::PermissionDenied,
                         "Nexus Mods did not accept the API key. Create a new personal API key on nexusmods.com.");
    }
    if (response->status == 429) {
        return makeError(ErrorCode::Unavailable, "Nexus Mods limits how often apps may ask. Try again later.");
    }
    if (!response->isSuccess()) {
        return makeError(ErrorCode::HttpStatus,
                         strings::concat("Nexus Mods answered HTTP ", response->status, "."), path);
    }
    return std::move(response->body);
}

Result<bool> NexusProvider::validateKey(const CancellationToken* cancel) {
    auto body = get("/v1/users/validate.json", 64 * 1024, cancel);
    if (!body) return std::move(body).error();
    auto parsed = json::parseBounded(body.value(), 64 * 1024);
    if (!parsed || !parsed->is_object()) return makeError(ErrorCode::ParseError, "Nexus Mods sent an unexpected answer.");
    const bool isPremium = json::getBool(parsed.value(), "is_premium").value_or(false);
    std::lock_guard<std::mutex> lock(mutex_);
    premium_ = isPremium;
    return isPremium;
}

Result<std::vector<ProviderGame>> NexusProvider::listGames(const CancellationToken* cancel) {
    std::vector<InstalledGameName> installed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        installed = installed_;
    }
    if (!premium()) (void)validateKey(cancel);  // only to know whether downloads will work
    auto body = get("/v1/games.json", kMaxGamesBytes, cancel);
    if (!body) return std::move(body).error();
    auto parsed = json::parseBounded(body.value(), kMaxGamesBytes);
    if (!parsed || !parsed->is_array()) return makeError(ErrorCode::ParseError, "Nexus Mods sent an unexpected game list.");
    std::vector<ProviderGame> games;
    std::map<std::string, std::string> titles;
    for (const auto& game : parsed.value()) {
        if (!game.is_object()) continue;
        const std::string domain = json::getString(game, "domain_name").value_or("");
        const std::string name = json::getString(game, "name").value_or("");
        if (!isDomain(domain) || name.empty()) continue;
        const std::string key = normalizeGameName(name);
        for (const auto& mine : installed) {
            if (key.empty() || normalizeGameName(mine.name) != key) continue;
            ProviderGame entry;
            entry.providerGameId = std::string(kNexusGamePrefix) + domain;
            entry.name = plainText(name, limits::kMaxDisplayStringBytes) + " (Nexus Mods)";
            entry.titleIds = {mine.titleId};
            entry.modCount = static_cast<int>(json::getInt(game, "mods").value_or(0));
            titles[domain] = mine.titleId;
            games.push_back(std::move(entry));
            break;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    titleIdByDomain_ = std::move(titles);
    return games;
}

Result<std::vector<ModSummary>> NexusProvider::collect(const std::string& domain, const CancellationToken* cancel) {
    std::vector<ModSummary> mods;
    std::set<std::string> seen;
    std::optional<Error> firstError;
    for (const char* list : {"trending", "latest_updated", "latest_added"}) {
        auto body = get(strings::concat("/v1/games/", domain, "/mods/", list, ".json"), kMaxListBytes, cancel);
        if (!body) {
            if (!firstError) firstError = body.error();
            continue;
        }
        auto parsed = json::parseBounded(body.value(), kMaxListBytes);
        if (!parsed || !parsed->is_array()) continue;
        for (const auto& item : parsed.value()) {
            auto summary = summaryFrom(item, domain);
            if (summary && seen.insert(summary->ref.modId).second) mods.push_back(std::move(summary).value());
        }
    }
    if (mods.empty() && firstError) return *firstError;
    return mods;
}

Result<ModPage> NexusProvider::browseMods(const SearchQuery& query, const CancellationToken* cancel) {
    auto domain = domainOf(query);
    if (!domain) return makeError(ErrorCode::InvalidArgument, "Not a Nexus Mods game.");
    auto mods = collect(*domain, cancel);
    if (!mods) return std::move(mods).error();
    ModPage page;
    page.total = mods->size();
    for (std::size_t i = query.offset; i < mods->size() && page.mods.size() < query.limit; ++i) {
        page.mods.push_back((*mods)[i]);
    }
    return page;
}

Result<ModPage> NexusProvider::searchMods(const SearchQuery& query, const CancellationToken* cancel) {
    // The public API has no search; the listed mods are filtered here.
    auto domain = domainOf(query);
    if (!domain) return makeError(ErrorCode::InvalidArgument, "Not a Nexus Mods game.");
    auto mods = collect(*domain, cancel);
    if (!mods) return std::move(mods).error();
    const std::string needle = strings::toLowerAscii(query.text);
    ModPage page;
    for (const auto& mod : mods.value()) {
        const std::string haystack = strings::toLowerAscii(mod.name + " " + mod.author + " " + mod.shortDescription);
        if (haystack.find(needle) == std::string::npos) continue;
        ++page.total;
        if (page.total > query.offset && page.mods.size() < query.limit) page.mods.push_back(mod);
    }
    return page;
}

Result<std::vector<ModFile>> NexusProvider::getFiles(const ModRef& ref, const CancellationToken* cancel) {
    auto parts = splitRef(ref);
    if (!parts) return makeError(ErrorCode::InvalidArgument, "Not a Nexus Mods mod.", ref.modId);
    auto body = get(strings::concat("/v1/games/", parts->first, "/mods/", parts->second, "/files.json"), kMaxListBytes,
                    cancel);
    if (!body) return std::move(body).error();
    auto parsed = json::parseBounded(body.value(), kMaxListBytes);
    const json::Json* list = parsed && parsed->is_object() ? json::getArray(parsed.value(), "files") : nullptr;
    if (list == nullptr) return makeError(ErrorCode::ParseError, "Nexus Mods sent an unexpected file list.");
    std::vector<ModFile> files;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        const std::string category = json::getString(item, "category_name").value_or("");
        if (category != "MAIN" && category != "OPTIONAL" && category != "UPDATE" && category != "MISCELLANEOUS") continue;
        const auto fileId = json::getInt(item, "file_id");
        const auto size = json::getInt(item, "size_in_bytes");
        const std::string format = formatOf(json::getString(item, "file_name").value_or(""));
        if (!fileId || *fileId <= 0 || !size || *size <= 0 || format.empty()) continue;  // rar etc. are not supported
        ModFile file;
        file.fileId = std::to_string(*fileId);
        file.displayName = plainText(json::getString(item, "name").value_or(file.fileId), 200) + " (" + category + ")";
        file.version = plainText(json::getString(item, "version").value_or(""), 64);
        file.sizeBytes = static_cast<std::uint64_t>(*size);
        file.format = format;
        file.primary = json::getBool(item, "is_primary").value_or(false);
        files.push_back(std::move(file));
    }
    // Main files first; the primary one (or the first main file) is what "Download" takes.
    std::stable_sort(files.begin(), files.end(), [](const ModFile& a, const ModFile& b) {
        const bool am = a.displayName.find("(MAIN)") != std::string::npos;
        const bool bm = b.displayName.find("(MAIN)") != std::string::npos;
        return am && !bm;
    });
    if (!files.empty() && std::none_of(files.begin(), files.end(), [](const ModFile& f) { return f.primary; })) {
        files.front().primary = true;
    }
    return files;
}

Result<ModDetails> NexusProvider::getModDetails(const ModRef& ref, const std::optional<GameContext>& game,
                                                const CancellationToken* cancel) {
    auto parts = splitRef(ref);
    if (!parts) return makeError(ErrorCode::InvalidArgument, "Not a Nexus Mods mod.", ref.modId);
    auto body = get(strings::concat("/v1/games/", parts->first, "/mods/", parts->second, ".json"), kMaxListBytes, cancel);
    if (!body) return std::move(body).error();
    auto parsed = json::parseBounded(body.value(), kMaxListBytes);
    if (!parsed) return makeError(ErrorCode::ParseError, "Nexus Mods sent an unexpected answer.");
    auto summary = summaryFrom(parsed.value(), parts->first);
    if (!summary) return makeError(ErrorCode::NotFound, "This mod is not available on Nexus Mods.");
    ModDetails details;
    details.summary = std::move(summary).value();
    details.description = plainText(json::getString(parsed.value(), "description").value_or(""), 6000);
    if (details.description.empty()) details.description = details.summary.shortDescription;
    details.homepage = strings::concat("https://www.nexusmods.com/", parts->first, "/mods/", parts->second);
    details.license = "See the mod page on Nexus Mods";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = titleIdByDomain_.find(parts->first); it != titleIdByDomain_.end()) details.titleIds = {it->second};
    }
    if (game && details.titleIds.empty()) details.titleIds = {game->titleId};
    if (!details.summary.thumbnailUrl.empty()) details.screenshots.push_back({details.summary.thumbnailUrl, ""});
    auto files = getFiles(ref, cancel);
    if (files) details.files = std::move(files).value();

    details.compatibilityReasons = {
        "Nexus Mods hosts mods for the PC version. This one is not tested on PS5.",
        "After the download Akeno checks every file: Windows programs, UE4SS, scripts loaders and console code are "
        "refused.",
    };
    const std::optional<bool> isPremium = premium();
    if (isPremium && !*isPremium) {
        details.compatibilityReasons.push_back(
            "Nexus Mods lets apps download only for Premium members. Your account can browse here, but not download.");
    }
    details.checks = {
        {"Source", "Nexus Mods (PC mod)", CompatibilityCheck::Mark::Warn},
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

Result<std::vector<Screenshot>> NexusProvider::getScreenshots(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->screenshots;
}

Result<std::vector<ModDependency>> NexusProvider::getDependencies(const ModRef&, const CancellationToken*) {
    return std::vector<ModDependency>{};  // the public API does not list requirements
}

Result<DownloadTicket> NexusProvider::resolveDownload(const ModRef& ref, const ModFile& file,
                                                      const CancellationToken* cancel) {
    auto parts = splitRef(ref);
    if (!parts || !isNumber(file.fileId)) return makeError(ErrorCode::InvalidArgument, "Not a Nexus Mods file.");
    auto body = get(strings::concat("/v1/games/", parts->first, "/mods/", parts->second, "/files/", file.fileId,
                                    "/download_link.json"),
                    256 * 1024, cancel);
    if (!body) {
        if (body.error().code == ErrorCode::HttpStatus || body.error().code == ErrorCode::PermissionDenied) {
            return makeError(ErrorCode::Unsupported,
                             "Nexus Mods gives download links to apps only for Premium members. Download this mod on "
                             "nexusmods.com instead, or use a Premium account.",
                             body.error().describe());
        }
        return std::move(body).error();
    }
    auto parsed = json::parseBounded(body.value(), 256 * 1024);
    if (!parsed || !parsed->is_array()) return makeError(ErrorCode::ParseError, "Nexus Mods sent an unexpected answer.");
    for (const auto& link : parsed.value()) {
        if (!link.is_object()) continue;
        auto uri = json::getString(link, "URI");
        if (!uri || !mods::isAllowedRemoteUrl(*uri) || uri->size() > 2048) continue;
        DownloadTicket ticket;
        ticket.url = *uri;
        ticket.expectedSize = file.sizeBytes;
        ticket.format = file.format;  // Nexus publishes no SHA-256; the size is checked, the hash recorded
        return ticket;
    }
    return makeError(ErrorCode::NotFound, "Nexus Mods offered no secure download link for this file.");
}

Result<std::string> NexusProvider::getLatestVersion(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->summary.version;
}

}  // namespace akeno::providers
