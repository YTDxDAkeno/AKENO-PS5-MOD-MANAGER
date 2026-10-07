// SPDX-License-Identifier: GPL-3.0-or-later
// GameBanana through its public API (apiv11, https://gamebanana.com/apiv11): free, no account,
// no key. Games are matched to installed games by name. Like Nexus, GameBanana hosts mods for
// the PC versions of games, so every mod is EXPERIMENTAL and the analysis after the download
// still refuses Windows code, script loaders and console code.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "akeno/network/Http.hpp"
#include "akeno/providers/IModProvider.hpp"
#include "akeno/providers/NexusProvider.hpp"  // InstalledGameName, normalizeGameName

namespace akeno::providers {

inline constexpr std::string_view kGameBananaProviderId = "gamebanana";
inline constexpr std::string_view kGameBananaGamePrefix = "gamebanana:";

class GameBananaProvider final : public IModProvider {
public:
    explicit GameBananaProvider(network::IHttpClient& http, std::string baseUrl = "https://gamebanana.com");

    std::string id() const override { return std::string(kGameBananaProviderId); }
    std::string displayName() const override { return "GameBanana"; }

    void setInstalledGames(std::vector<InstalledGameName> games);

    Result<std::vector<ProviderGame>> listGames(const CancellationToken* cancel) override;
    Result<ModPage> searchMods(const SearchQuery& query, const CancellationToken* cancel) override;
    Result<ModPage> browseMods(const SearchQuery& query, const CancellationToken* cancel) override;
    Result<ModDetails> getModDetails(const ModRef& ref, const std::optional<GameContext>& game,
                                     const CancellationToken* cancel) override;
    Result<std::vector<ModFile>> getFiles(const ModRef& ref, const CancellationToken* cancel) override;
    Result<std::vector<Screenshot>> getScreenshots(const ModRef& ref, const CancellationToken* cancel) override;
    Result<std::vector<ModDependency>> getDependencies(const ModRef& ref, const CancellationToken* cancel) override;
    Result<DownloadTicket> resolveDownload(const ModRef& ref, const ModFile& file,
                                           const CancellationToken* cancel) override;
    Result<std::string> getLatestVersion(const ModRef& ref, const CancellationToken* cancel) override;

private:
    Result<std::string> get(const std::string& path, const CancellationToken* cancel);
    Result<ModPage> list(const SearchQuery& query, const CancellationToken* cancel);

    network::IHttpClient& http_;
    std::string baseUrl_;
    std::mutex mutex_;
    std::vector<InstalledGameName> installed_;
    std::map<std::string, std::string> titleIdByGame_;  // GameBanana game id -> title ID
};

}  // namespace akeno::providers
