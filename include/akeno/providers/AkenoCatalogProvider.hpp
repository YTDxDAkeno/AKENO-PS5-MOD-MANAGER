// SPDX-License-Identifier: GPL-3.0-or-later
// The Akeno Catalogue: a curated, PS5-focused list of mods published as static JSON
// (docs/mod-format.md). Paths are built only from validated identifiers.
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "akeno/mods/Catalog.hpp"
#include "akeno/network/Http.hpp"
#include "akeno/providers/IModProvider.hpp"

namespace akeno::providers {

inline constexpr std::string_view kAkenoCatalogId = "akeno-catalogue";

// "<game-id>/<mod-id>" <-> ModRef::modId
std::string catalogModId(std::string_view gameId, std::string_view modId);
bool splitCatalogModId(std::string_view combined, std::string& gameId, std::string& modId);

class AkenoCatalogProvider final : public IModProvider {
public:
    // `baseUrl` must be https (or http on loopback for local testing) and end with '/'.
    static Result<std::unique_ptr<AkenoCatalogProvider>> create(network::IHttpClient& http, std::string baseUrl,
                                                               std::chrono::seconds cacheTtl = std::chrono::minutes(10));

    std::string id() const override { return std::string(kAkenoCatalogId); }
    std::string displayName() const override { return "Akeno Catalogue"; }

    Result<std::vector<ProviderGame>> listGames(const CancellationToken* cancel) override;
    Result<ModPage> searchMods(const SearchQuery& query, const CancellationToken* cancel) override;
    Result<ModPage> browseMods(const SearchQuery& query, const CancellationToken* cancel) override;
    Result<ModDetails> getModDetails(const ModRef& ref, const std::optional<GameContext>& game,
                                     const CancellationToken* cancel) override;
    Result<std::vector<ModFile>> getFiles(const ModRef& ref, const CancellationToken* cancel) override;
    Result<std::vector<Screenshot>> getScreenshots(const ModRef& ref, const CancellationToken* cancel) override;
    Result<std::vector<ModDependency>> getDependencies(const ModRef& ref, const CancellationToken* cancel) override;
    Result<DownloadTicket> resolveDownload(const ModRef& ref, const ModFile& file, const CancellationToken* cancel) override;
    Result<std::string> getLatestVersion(const ModRef& ref, const CancellationToken* cancel) override;

    // Catalogue-specific access used by the install pipeline.
    Result<mods::ModManifest> manifest(const ModRef& ref, const CancellationToken* cancel);
    Result<mods::CatalogIndex> index(const CancellationToken* cancel);
    void clearCache();
    const std::string& baseUrl() const { return baseUrl_; }

private:
    AkenoCatalogProvider(network::IHttpClient& http, std::string baseUrl, std::chrono::seconds ttl)
        : http_(http), baseUrl_(std::move(baseUrl)), ttl_(ttl) {}

    Result<std::string> fetch(const std::string& relativePath, std::size_t maxBytes, const CancellationToken* cancel);
    Result<mods::CatalogGame> game(const std::string& gameId, const CancellationToken* cancel);
    Result<mods::ModManifest> manifest(const std::string& gameId, const std::string& modId,
                                       const CancellationToken* cancel);
    Result<std::string> resolveGameId(const SearchQuery& query, const CancellationToken* cancel);
    Result<ModPage> list(const SearchQuery& query, bool search, const CancellationToken* cancel);

    template <typename T>
    struct Cached {
        T value;
        std::chrono::steady_clock::time_point fetchedAt;
    };

    network::IHttpClient& http_;
    std::string baseUrl_;
    std::chrono::seconds ttl_;
    std::mutex mutex_;
    std::optional<Cached<mods::CatalogIndex>> index_;
    std::map<std::string, Cached<mods::CatalogGame>> games_;
    std::map<std::string, Cached<mods::ModManifest>> manifests_;
};

}  // namespace akeno::providers
