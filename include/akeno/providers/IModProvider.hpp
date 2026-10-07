// SPDX-License-Identifier: GPL-3.0-or-later
// Mod provider interface (Phase 2+). Every source of mods — the Akeno catalogue, GitHub
// releases, Nexus Mods, mod.io — implements this interface, so no part of the application is
// tied to one provider. 0.1.0-alpha ships the interface only; no provider is implemented yet.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"

namespace akeno::providers {

// Compatibility labels. Nothing is ever labelled Verified without a matching catalogue record
// for both the title ID and the installed game version (safety model rule H4).
enum class CompatibilityStatus { Verified, Likely, Experimental, Unknown, PcOnly, Incompatible };

struct ModRef {
    std::string providerId;  // "akeno-catalogue", "nexus", "modio", "github"
    std::string modId;       // provider-specific, validated by the provider
};

struct ModSummary {
    ModRef ref;
    std::string name;
    std::string author;
    std::string version;
    std::string shortDescription;
    std::string thumbnailUrl;
    CompatibilityStatus compatibility = CompatibilityStatus::Unknown;
    std::optional<double> rating;           // 0..5 when the provider has ratings
    std::optional<std::uint64_t> downloadSize;
};

struct ModFile {
    std::string fileId;
    std::string displayName;
    std::string version;
    std::uint64_t sizeBytes = 0;
    std::string sha256;                     // lower-case hex; empty if the provider has none
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

struct ModDetails {
    ModSummary summary;
    std::string description;
    std::vector<std::string> titleIds;      // games this mod applies to
    std::vector<std::string> gameVersions;  // versions the mod was verified against
    std::string engine;                     // "unreal", "unknown", ...
    std::string modType;                    // "asset-replacement", ...
    std::vector<ModDependency> dependencies;
};

enum class BrowseOrder { Featured, Popular, Newest, Verified };

struct SearchQuery {
    std::string titleId;
    std::string text;                       // empty = browse
    BrowseOrder order = BrowseOrder::Featured;
    std::size_t offset = 0;
    std::size_t limit = 24;
};

struct DownloadTicket {
    std::string url;                        // https only
    std::uint64_t expectedSize = 0;
    std::string expectedSha256;
    std::vector<std::pair<std::string, std::string>> headers;  // e.g. provider authentication
};

class IModProvider {
public:
    virtual ~IModProvider() = default;

    virtual std::string id() const = 0;
    virtual std::string displayName() const = 0;

    virtual Result<std::vector<ModSummary>> searchMods(const SearchQuery& query, const CancellationToken* cancel) = 0;
    virtual Result<std::vector<ModSummary>> browseMods(const SearchQuery& query, const CancellationToken* cancel) = 0;
    virtual Result<ModDetails> getModDetails(const ModRef& ref, const CancellationToken* cancel) = 0;
    virtual Result<std::vector<ModFile>> getFiles(const ModRef& ref, const CancellationToken* cancel) = 0;
    virtual Result<std::vector<Screenshot>> getScreenshots(const ModRef& ref, const CancellationToken* cancel) = 0;
    virtual Result<std::vector<ModDependency>> getDependencies(const ModRef& ref, const CancellationToken* cancel) = 0;
    // Resolves a file to a concrete HTTPS download. Providers that restrict downloads (for
    // example to premium accounts) return Unsupported with an explanation; they never bypass
    // the restriction.
    virtual Result<DownloadTicket> resolveDownload(const ModRef& ref, const ModFile& file,
                                                   const CancellationToken* cancel) = 0;
    virtual Result<std::string> getLatestVersion(const ModRef& ref, const CancellationToken* cancel) = 0;
};

}  // namespace akeno::providers
