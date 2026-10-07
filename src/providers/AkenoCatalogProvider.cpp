// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/providers/AkenoCatalogProvider.hpp"

#include <algorithm>

#include "akeno/compatibility/CompatibilityRules.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/network/Url.hpp"

namespace akeno::providers {

namespace {

int statusRank(CompatibilityStatus status) {
    switch (status) {
        case CompatibilityStatus::Verified: return 0;
        case CompatibilityStatus::Likely: return 1;
        case CompatibilityStatus::Experimental: return 2;
        case CompatibilityStatus::Unknown: return 3;
        case CompatibilityStatus::PcOnly: return 4;
        case CompatibilityStatus::Incompatible: return 5;
    }
    return 3;
}

CompatibilityCheck::Mark toMark(compatibility::CheckMark mark) {
    switch (mark) {
        case compatibility::CheckMark::Pass: return CompatibilityCheck::Mark::Pass;
        case compatibility::CheckMark::Warn: return CompatibilityCheck::Mark::Warn;
        case compatibility::CheckMark::Fail: return CompatibilityCheck::Mark::Fail;
        case compatibility::CheckMark::Unknown: return CompatibilityCheck::Mark::Unknown;
    }
    return CompatibilityCheck::Mark::Unknown;
}

bool matches(const mods::ModSummaryEntry& entry, const std::string& needle) {
    auto has = [&](const std::string& field) { return strings::toLowerAscii(field).find(needle) != std::string::npos; };
    if (has(entry.name) || has(entry.author) || has(entry.summary)) return true;
    return std::any_of(entry.categories.begin(), entry.categories.end(), has);
}

}  // namespace

std::string catalogModId(std::string_view gameId, std::string_view modId) {
    return std::string(gameId) + "/" + std::string(modId);
}

bool splitCatalogModId(std::string_view combined, std::string& gameId, std::string& modId) {
    std::size_t slash = combined.find('/');
    if (slash == std::string_view::npos) return false;
    std::string_view game = combined.substr(0, slash);
    std::string_view mod = combined.substr(slash + 1);
    if (!mods::isValidCatalogId(game) || !mods::isValidCatalogId(mod)) return false;
    gameId = std::string(game);
    modId = std::string(mod);
    return true;
}

Result<std::unique_ptr<AkenoCatalogProvider>> AkenoCatalogProvider::create(network::IHttpClient& http,
                                                                           std::string baseUrl,
                                                                           std::chrono::seconds cacheTtl) {
    if (!mods::isAllowedRemoteUrl(baseUrl)) {
        return makeError(ErrorCode::InvalidArgument, "The catalogue address must be an https:// URL.", baseUrl);
    }
    auto parsed = network::parseUrl(baseUrl);
    if (!parsed || parsed->target.find('?') != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument, "The catalogue address must not contain a query.", baseUrl);
    }
    if (baseUrl.back() != '/') baseUrl.push_back('/');
    return std::unique_ptr<AkenoCatalogProvider>(new AkenoCatalogProvider(http, std::move(baseUrl), cacheTtl));
}

void AkenoCatalogProvider::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.reset();
    games_.clear();
    manifests_.clear();
}

Result<std::string> AkenoCatalogProvider::fetch(const std::string& relativePath, std::size_t maxBytes,
                                                const CancellationToken* cancel) {
    network::HttpRequest request;
    request.url = baseUrl_ + relativePath;
    request.maxResponseBytes = maxBytes;
    request.headers = {{"Accept", "application/json"}};
    request.totalTimeoutMs = 20000;
    auto response = http_.send(request, cancel);
    if (!response) {
        Error error = std::move(response).error();
        if (error.code != ErrorCode::Cancelled && error.code != ErrorCode::SafetyViolation) {
            error.message = "The mod catalogue could not be loaded. " + error.message;
        }
        return error;
    }
    if (response->status == 404) {
        return makeError(ErrorCode::NotFound, "The mod catalogue does not have this entry.", request.url);
    }
    if (!response->isSuccess()) {
        return makeError(ErrorCode::HttpStatus, "The mod catalogue server returned an error.",
                         strings::concat("HTTP ", response->status, " for ", request.url));
    }
    return std::move(response->body);
}

Result<mods::CatalogIndex> AkenoCatalogProvider::index(const CancellationToken* cancel) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index_ && std::chrono::steady_clock::now() - index_->fetchedAt < ttl_) return index_->value;
    }
    auto body = fetch("index.json", mods::kMaxIndexBytes, cancel);
    if (!body) return std::move(body).error();
    auto parsed = mods::parseCatalogIndex(body.value());
    if (!parsed) return std::move(parsed).error();
    std::lock_guard<std::mutex> lock(mutex_);
    index_ = Cached<mods::CatalogIndex>{parsed.value(), std::chrono::steady_clock::now()};
    return parsed;
}

Result<mods::CatalogGame> AkenoCatalogProvider::game(const std::string& gameId, const CancellationToken* cancel) {
    if (!mods::isValidCatalogId(gameId)) {
        return makeError(ErrorCode::InvalidArgument, "Invalid catalogue game identifier.", gameId);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = games_.find(gameId);
        if (it != games_.end() && std::chrono::steady_clock::now() - it->second.fetchedAt < ttl_) return it->second.value;
    }
    auto body = fetch("games/" + gameId + ".json", mods::kMaxGameFileBytes, cancel);
    if (!body) return std::move(body).error();
    auto parsed = mods::parseCatalogGame(body.value(), gameId);
    if (!parsed) return std::move(parsed).error();
    std::lock_guard<std::mutex> lock(mutex_);
    games_[gameId] = Cached<mods::CatalogGame>{parsed.value(), std::chrono::steady_clock::now()};
    return parsed;
}

Result<mods::ModManifest> AkenoCatalogProvider::manifest(const std::string& gameId, const std::string& modId,
                                                         const CancellationToken* cancel) {
    if (!mods::isValidCatalogId(gameId) || !mods::isValidCatalogId(modId)) {
        return makeError(ErrorCode::InvalidArgument, "Invalid catalogue mod identifier.", gameId + "/" + modId);
    }
    const std::string key = catalogModId(gameId, modId);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = manifests_.find(key);
        if (it != manifests_.end() && std::chrono::steady_clock::now() - it->second.fetchedAt < ttl_) {
            return it->second.value;
        }
    }
    auto body = fetch("mods/" + gameId + "/" + modId + ".json", mods::kMaxManifestBytes, cancel);
    if (!body) return std::move(body).error();
    auto parsed = mods::parseModManifest(body.value(), modId);
    if (!parsed) return std::move(parsed).error();
    std::lock_guard<std::mutex> lock(mutex_);
    manifests_[key] = Cached<mods::ModManifest>{parsed.value(), std::chrono::steady_clock::now()};
    return parsed;
}

Result<mods::ModManifest> AkenoCatalogProvider::manifest(const ModRef& ref, const CancellationToken* cancel) {
    std::string gameId;
    std::string modId;
    if (ref.providerId != kAkenoCatalogId || !splitCatalogModId(ref.modId, gameId, modId)) {
        return makeError(ErrorCode::InvalidArgument, "This mod does not belong to the Akeno Catalogue.", ref.modId);
    }
    return manifest(gameId, modId, cancel);
}

Result<std::vector<ProviderGame>> AkenoCatalogProvider::listGames(const CancellationToken* cancel) {
    auto idx = index(cancel);
    if (!idx) return std::move(idx).error();
    std::vector<ProviderGame> games;
    for (const auto& ref : idx->games) {
        games.push_back({ref.id, ref.name, ref.titleIds, ref.modCount});
    }
    return games;
}

Result<std::string> AkenoCatalogProvider::resolveGameId(const SearchQuery& query, const CancellationToken* cancel) {
    if (!query.providerGameId.empty()) {
        if (!mods::isValidCatalogId(query.providerGameId)) {
            return makeError(ErrorCode::InvalidArgument, "Invalid catalogue game identifier.", query.providerGameId);
        }
        return query.providerGameId;
    }
    if (!query.game) {
        return makeError(ErrorCode::InvalidArgument, "No game was selected.");
    }
    auto idx = index(cancel);
    if (!idx) return std::move(idx).error();
    const mods::CatalogGameRef* ref = idx->findByTitleId(query.game->titleId);
    if (ref == nullptr) {
        return makeError(ErrorCode::NotFound, "The Akeno Catalogue has no mods for this game yet.", query.game->titleId);
    }
    return ref->id;
}

Result<ModPage> AkenoCatalogProvider::list(const SearchQuery& query, bool search, const CancellationToken* cancel) {
    auto gameId = resolveGameId(query, cancel);
    if (!gameId) return std::move(gameId).error();
    auto catalogGame = game(gameId.value(), cancel);
    if (!catalogGame) return std::move(catalogGame).error();

    const std::string needle = strings::toLowerAscii(strings::truncateUtf8(strings::trim(query.text), 64));
    std::vector<ModSummary> all;
    for (const auto& entry : catalogGame->mods) {
        if (search && !needle.empty() && !matches(entry, needle)) continue;
        ModSummary summary;
        summary.ref = ModRef{std::string(kAkenoCatalogId), catalogModId(catalogGame->id, entry.id)};
        summary.name = entry.name;
        summary.author = entry.author;
        summary.version = entry.version;
        summary.shortDescription = entry.summary;
        summary.categories = entry.categories;
        summary.thumbnailUrl = entry.thumbnailUrl;
        summary.downloadSize = entry.downloadSize;
        summary.updatedAt = entry.updatedAt;
        summary.compatibility = compatibility::summaryLabel(entry, query.game);
        all.push_back(std::move(summary));
    }
    switch (query.order) {
        case BrowseOrder::Featured:
        case BrowseOrder::Popular:  // the catalogue has no download statistics: curated order
            break;
        case BrowseOrder::Newest:
            std::stable_sort(all.begin(), all.end(),
                             [](const ModSummary& a, const ModSummary& b) { return a.updatedAt > b.updatedAt; });
            break;
        case BrowseOrder::Verified:
            std::stable_sort(all.begin(), all.end(), [](const ModSummary& a, const ModSummary& b) {
                return statusRank(a.compatibility) < statusRank(b.compatibility);
            });
            break;
    }
    ModPage page;
    page.total = all.size();
    const std::size_t limit = std::min<std::size_t>(query.limit == 0 ? 50 : query.limit, 200);
    for (std::size_t i = query.offset; i < all.size() && page.mods.size() < limit; ++i) {
        page.mods.push_back(std::move(all[i]));
    }
    return page;
}

Result<ModPage> AkenoCatalogProvider::searchMods(const SearchQuery& query, const CancellationToken* cancel) {
    return list(query, true, cancel);
}

Result<ModPage> AkenoCatalogProvider::browseMods(const SearchQuery& query, const CancellationToken* cancel) {
    return list(query, false, cancel);
}

Result<ModDetails> AkenoCatalogProvider::getModDetails(const ModRef& ref, const std::optional<GameContext>& game,
                                                       const CancellationToken* cancel) {
    auto m = manifest(ref, cancel);
    if (!m) return std::move(m).error();
    const mods::ModManifest& manifest = m.value();
    ModDetails details;
    details.summary.ref = ref;
    details.summary.name = manifest.name;
    details.summary.author = manifest.author;
    details.summary.version = manifest.version;
    details.summary.shortDescription = manifest.summary;
    details.summary.categories = manifest.categories;
    details.summary.thumbnailUrl = manifest.thumbnailUrl;
    details.summary.downloadSize = manifest.downloadSize;
    details.summary.updatedAt = manifest.updatedAt;
    details.description = manifest.description;
    details.license = manifest.license;
    details.homepage = manifest.homepage;
    details.titleIds = manifest.titleIds;
    details.gameVersions = manifest.gameVersions;
    details.engine = manifest.engine;
    details.modType = manifest.modTypeText;
    details.files.push_back(ModFile{"main", manifest.name, manifest.version, manifest.downloadSize,
                                    manifest.downloadSha256, std::string(mods::toString(manifest.format)), true});
    for (const auto& shot : manifest.screenshots) details.screenshots.push_back({shot.url, shot.caption});
    auto addDeps = [&](const std::vector<std::string>& ids, ModDependency::Kind kind) {
        std::string gameId;
        std::string modId;
        splitCatalogModId(ref.modId, gameId, modId);
        for (const auto& id : ids) {
            details.dependencies.push_back({kind, ModRef{std::string(kAkenoCatalogId), catalogModId(gameId, id)}});
        }
    };
    addDeps(manifest.requires_, ModDependency::Kind::Requires);
    addDeps(manifest.recommends, ModDependency::Kind::Recommends);
    addDeps(manifest.conflictsWith, ModDependency::Kind::ConflictsWith);
    addDeps(manifest.loadAfter, ModDependency::Kind::LoadAfter);
    addDeps(manifest.loadBefore, ModDependency::Kind::LoadBefore);

    compatibility::CompatibilityResult result = compatibility::evaluateManifest(manifest, game);
    details.summary.compatibility = result.status;
    for (const auto& line : result.lines) details.checks.push_back({line.label, line.value, toMark(line.mark)});
    details.compatibilityReasons = result.reasons;
    details.risk = std::string(compatibility::toString(result.risk));
    details.installable = result.installable;
    details.needsConfirmation = result.needsConfirmation;
    return details;
}

Result<std::vector<ModFile>> AkenoCatalogProvider::getFiles(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->files;
}

Result<std::vector<Screenshot>> AkenoCatalogProvider::getScreenshots(const ModRef& ref, const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->screenshots;
}

Result<std::vector<ModDependency>> AkenoCatalogProvider::getDependencies(const ModRef& ref,
                                                                         const CancellationToken* cancel) {
    auto details = getModDetails(ref, std::nullopt, cancel);
    if (!details) return std::move(details).error();
    return details->dependencies;
}

Result<DownloadTicket> AkenoCatalogProvider::resolveDownload(const ModRef& ref, const ModFile& file,
                                                             const CancellationToken* cancel) {
    auto m = manifest(ref, cancel);
    if (!m) return std::move(m).error();
    if (file.fileId != "main") {
        return makeError(ErrorCode::NotFound, "This mod has no such file.", file.fileId);
    }
    DownloadTicket ticket;
    ticket.url = m->downloadUrl;
    ticket.expectedSize = m->downloadSize;
    ticket.expectedSha256 = m->downloadSha256;
    ticket.format = std::string(mods::toString(m->format));
    return ticket;
}

Result<std::string> AkenoCatalogProvider::getLatestVersion(const ModRef& ref, const CancellationToken* cancel) {
    std::string gameId;
    std::string modId;
    if (ref.providerId != kAkenoCatalogId || !splitCatalogModId(ref.modId, gameId, modId)) {
        return makeError(ErrorCode::InvalidArgument, "This mod does not belong to the Akeno Catalogue.", ref.modId);
    }
    auto catalogGame = game(gameId, cancel);
    if (!catalogGame) return std::move(catalogGame).error();
    for (const auto& entry : catalogGame->mods) {
        if (entry.id == modId) return entry.version;
    }
    return makeError(ErrorCode::NotFound, "The mod is no longer in the catalogue.", ref.modId);
}

}  // namespace akeno::providers
