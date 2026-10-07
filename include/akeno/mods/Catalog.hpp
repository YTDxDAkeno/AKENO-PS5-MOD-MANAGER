// SPDX-License-Identifier: GPL-3.0-or-later
// Akeno Catalogue documents, schema version 1 (docs/mod-format.md).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Result.hpp"

namespace akeno::mods {

inline constexpr int kCatalogSchemaVersion = 1;
inline constexpr std::size_t kMaxIndexBytes = 1024 * 1024;
inline constexpr std::size_t kMaxGameFileBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxManifestBytes = 256 * 1024;
inline constexpr std::uint64_t kMaxDownloadSize = 64ull * 1024 * 1024 * 1024;

// ^[a-z0-9][a-z0-9-]{0,62}$
bool isValidCatalogId(std::string_view id) noexcept;
// 1..32 characters of [0-9A-Za-z.+-]
bool isValidModVersion(std::string_view version) noexcept;
// https:// URLs, or http:// to a loopback host (local development only).
bool isAllowedRemoteUrl(std::string_view url) noexcept;

enum class ClaimedStatus { Verified, Likely, Experimental, Unknown, PcOnly, Incompatible };
ClaimedStatus parseClaimedStatus(std::string_view text) noexcept;  // unknown strings -> Unknown
std::string_view toString(ClaimedStatus status) noexcept;

enum class ModType { AssetReplacement, AssetAddition, Config, TestHarmless, Other };
ModType parseModType(std::string_view text) noexcept;
std::string_view toString(ModType type) noexcept;
bool isInstallableType(ModType type) noexcept;

enum class ArchiveFormat { Zip, Tar, TarGz, SevenZip, Unknown };
ArchiveFormat parseArchiveFormat(std::string_view text) noexcept;
std::string_view toString(ArchiveFormat format) noexcept;

struct CatalogGameRef {
    std::string id;
    std::string name;
    std::vector<std::string> titleIds;
    int modCount = 0;
};

struct CatalogIndex {
    std::string name;
    std::string updatedAt;
    std::vector<CatalogGameRef> games;

    const CatalogGameRef* findByTitleId(std::string_view titleId) const;
    const CatalogGameRef* findById(std::string_view id) const;
};

struct ModSummaryEntry {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string summary;
    std::vector<std::string> categories;
    ClaimedStatus claimed = ClaimedStatus::Unknown;
    std::vector<std::string> gameVersions;  // optional copy of the manifest's game.versions
    std::string thumbnailUrl;
    std::optional<std::uint64_t> downloadSize;
    std::string updatedAt;
};

struct CatalogGame {
    std::string id;
    std::string name;
    std::vector<std::string> titleIds;
    std::string engine;
    std::vector<ModSummaryEntry> mods;
};

struct Screenshot {
    std::string url;
    std::string caption;
};

struct ModManifest {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string summary;
    std::string description;
    std::vector<std::string> categories;
    std::string license;
    std::string homepage;
    std::string updatedAt;

    std::vector<std::string> titleIds;
    std::vector<std::string> gameVersions;
    std::string platform;

    ClaimedStatus claimed = ClaimedStatus::Unknown;
    std::string engine;
    ModType modType = ModType::Other;
    std::string modTypeText;
    std::string compatibilityNotes;

    std::string downloadUrl;
    std::uint64_t downloadSize = 0;
    std::string downloadSha256;
    ArchiveFormat format = ArchiveFormat::Unknown;

    std::string installMethod;
    std::string archiveRoot;   // relative, validated
    std::string targetPrefix;  // relative, validated

    std::string thumbnailUrl;
    std::vector<Screenshot> screenshots;

    std::vector<std::string> requires_;
    std::vector<std::string> recommends;
    std::vector<std::string> conflictsWith;
    std::vector<std::string> loadAfter;
    std::vector<std::string> loadBefore;
};

// Parsers. Errors name the offending field ("download.sha256: ...").
Result<CatalogIndex> parseCatalogIndex(std::string_view json);
Result<CatalogGame> parseCatalogGame(std::string_view json, std::string_view expectedId);
Result<ModManifest> parseModManifest(std::string_view json, std::string_view expectedId);

// Validates a relative path used inside archives/overlays: no absolute paths, no "..", no
// empty or "." components, no backslashes, no NUL, at most 512 bytes. "" is allowed.
bool isSafeRelativePath(std::string_view path) noexcept;

}  // namespace akeno::mods
