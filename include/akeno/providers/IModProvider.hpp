// SPDX-License-Identifier: GPL-3.0-or-later
// Mod provider interface. Every source of mods — the Akeno catalogue, GitHub releases,
// Nexus Mods, mod.io — implements it, so no part of the application is tied to one provider.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"

namespace akeno::providers {

// Labels shown to the user. Nothing is ever labelled Verified without a matching record for
// both the title ID and the installed game version (safety model rule H4).
enum class CompatibilityStatus { Verified, Likely, Experimental, Unknown, PcOnly, Incompatible };

struct ModRef {
    std::string providerId;  // "akeno-catalogue", "nexus", "modio", "github"
    std::string modId;       // provider-specific, validated by the provider

    bool operator==(const ModRef&) const = default;
};

// The installed game a request is about; labels depend on its version.
struct GameContext {
    std::string titleId;
    std::string version;  // empty when unknown
};

struct ProviderGame {
    std::string providerGameId;
    std::string name;
    std::vector<std::string> titleIds;
    int modCount = 0;
};

struct ModSummary {
    ModRef ref;
    std::string name;
    std::string author;
    std::string version;
    std::string shortDescription;
    std::vector<std::string> categories;
    std::string thumbnailUrl;
    CompatibilityStatus compatibility = CompatibilityStatus::Unknown;
    std::optional<double> rating;           // 0..5 when the provider has ratings
    std::optional<std::uint64_t> downloadSize;
    std::string updatedAt;
};

struct ModFile {
    std::string fileId;
    std::string displayName;
    std::string version;
    std::uint64_t sizeBytes = 0;
    std::string sha256;                     // lower-case hex; empty if the provider has none
    std::string format;                     // "zip", "tar", "tar.gz", ...
    bool primary = false;
};

struct ModDependency {
    enum class Kind { Requires, Recommends, ConflictsWith, LoadAfter, LoadBefore };
    Kind kind = Kind::Requires;
    ModRef target;
};

struct Screenshot {
    std::string url;
    std::string caption;
};

struct CompatibilityCheck {
    enum class Mark { Pass, Warn, Fail, Unknown };
    std::string label;
    std::string value;
    Mark mark = Mark::Unknown;
};

struct ModDetails {
    ModSummary summary;
    std::string description;
    std::string license;
    std::string homepage;
    std::vector<std::string> titleIds;      // games this mod applies to
    std::vector<std::string> gameVersions;  // versions the mod was checked against
    std::string engine;                     // "unreal", ...
    std::string modType;                    // "asset-replacement", ...
    std::vector<ModFile> files;
    std::vector<Screenshot> screenshots;
    std::vector<ModDependency> dependencies;

    // Evaluated against the GameContext of the request.
    std::vector<CompatibilityCheck> checks;
    std::vector<std::string> compatibilityReasons;
    std::string risk;                       // "LOW", "MEDIUM", "HIGH", "UNKNOWN"
    bool installable = false;               // allowed by the compatibility rules
    bool needsConfirmation = false;         // experimental: requires a deliberate confirmation
};

enum class BrowseOrder { Featured, Popular, Newest, Verified };

struct SearchQuery {
    std::string providerGameId;             // or empty to use the game context's title ID
    std::optional<GameContext> game;
    std::string text;                       // empty = browse
    BrowseOrder order = BrowseOrder::Featured;
    std::size_t offset = 0;
    std::size_t limit = 50;
};

struct ModPage {
    std::vector<ModSummary> mods;
    std::size_t total = 0;                  // matches before paging
};

struct DownloadTicket {
    std::string url;                        // https only
    std::uint64_t expectedSize = 0;
    std::string expectedSha256;
    std::string format;
    std::vector<std::pair<std::string, std::string>> headers;  // e.g. provider authentication
};

class IModProvider {
public:
    virtual ~IModProvider() = default;

    virtual std::string id() const = 0;
    virtual std::string displayName() const = 0;

    virtual Result<std::vector<ProviderGame>> listGames(const CancellationToken* cancel) = 0;
    virtual Result<ModPage> searchMods(const SearchQuery& query, const CancellationToken* cancel) = 0;
    virtual Result<ModPage> browseMods(const SearchQuery& query, const CancellationToken* cancel) = 0;
    virtual Result<ModDetails> getModDetails(const ModRef& ref, const std::optional<GameContext>& game,
                                             const CancellationToken* cancel) = 0;
    virtual Result<std::vector<ModFile>> getFiles(const ModRef& ref, const CancellationToken* cancel) = 0;
    virtual Result<std::vector<Screenshot>> getScreenshots(const ModRef& ref, const CancellationToken* cancel) = 0;
    virtual Result<std::vector<ModDependency>> getDependencies(const ModRef& ref, const CancellationToken* cancel) = 0;
    // Resolves a file to a concrete HTTPS download. Providers that restrict downloads (for
    // example to premium accounts) return Unsupported with an explanation; they never bypass
    // the restriction. The download itself is performed by Akeno's download engine.
    virtual Result<DownloadTicket> resolveDownload(const ModRef& ref, const ModFile& file,
                                                   const CancellationToken* cancel) = 0;
    virtual Result<std::string> getLatestVersion(const ModRef& ref, const CancellationToken* cancel) = 0;
};

}  // namespace akeno::providers
