// SPDX-License-Identifier: GPL-3.0-or-later
// Nexus Mods through its official public API (v1, https://api.nexusmods.com), with the user's
// own personal API key. No scraping, no built-in key, no way around Nexus's rules: the API gives
// download links only to Premium members, and everyone else is told so.
//
// Nexus has no PS5 section. Its games are matched to installed games by name, and every mod is
// a PC mod: it is labelled EXPERIMENTAL (download and install need a deliberate confirmation),
// and the analysis after the download still refuses Windows code, UE4SS, console code etc.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "akeno/network/Http.hpp"
#include "akeno/providers/IModProvider.hpp"

namespace akeno::providers {

inline constexpr std::string_view kNexusProviderId = "nexus";
inline constexpr std::string_view kNexusGamePrefix = "nexus:";  // ProviderGame / SearchQuery ids

struct InstalledGameName {
    std::string titleId;
    std::string name;
};

// Lower-case letters and digits only: "MONSTER HUNTER STORIES 3: TWISTED REFLECTION" and
// "Monster Hunter Stories 3 Twisted Reflection" match.
std::string normalizeGameName(std::string_view name);
// A personal API key as Nexus issues it; anything else is refused before it is sent anywhere.
bool isPlausibleNexusKey(std::string_view key);

class NexusProvider final : public IModProvider {
public:
    NexusProvider(network::IHttpClient& http, std::string apiKey, std::string baseUrl = "https://api.nexusmods.com");

    std::string id() const override { return std::string(kNexusProviderId); }
    std::string displayName() const override { return "Nexus Mods"; }

    // The installed games to match Nexus games against (by name).
    void setInstalledGames(std::vector<InstalledGameName> games);
    // Checks the key; true for Premium members (who may download through apps).
    Result<bool> validateKey(const CancellationToken* cancel);
    std::optional<bool> premium() const;

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
    Result<std::string> get(const std::string& path, std::size_t maxBytes, const CancellationToken* cancel);
    Result<std::vector<ModSummary>> collect(const std::string& domain, const CancellationToken* cancel);

    network::IHttpClient& http_;
    std::string apiKey_;
    std::string baseUrl_;
    mutable std::mutex mutex_;
    std::vector<InstalledGameName> installed_;
    std::optional<bool> premium_;
    std::map<std::string, std::string> titleIdByDomain_;  // filled by listGames
};

}  // namespace akeno::providers
