// SPDX-License-Identifier: GPL-3.0-or-later
// Reads untrusted mod archives (zip, tar, tar.gz, 7z) with libarchive and extracts them into a
// fresh staging directory. The whole archive is refused if any entry is unsafe; nothing is
// ever written outside the staging directory, and nothing from the archive is executed.
//
// Refused: absolute paths, "..", ".", empty components, backslashes, colons, control
// characters, names that are not UTF-8, over-long names or paths, too deep nesting, duplicate
// names, a file and a directory with the same name, symlinks, hard links, devices, FIFOs,
// sockets, encrypted entries, and archives that exceed the size, count or compression-ratio
// limits. Permissions, owners and timestamps from the archive are ignored.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/mods/Catalog.hpp"
#include "akeno/security/SafeFs.hpp"

namespace akeno::archives {

struct ExtractionLimits {
    std::size_t maxEntries = 20000;
    std::uint64_t maxEntryBytes = 4ull * 1024 * 1024 * 1024;
    std::uint64_t maxTotalBytes = 16ull * 1024 * 1024 * 1024;
    std::size_t maxPathBytes = limits::kMaxPathBytes;
    std::size_t maxComponentBytes = limits::kMaxFileNameBytes;
    int maxDepth = 32;
    // Archive bombs: above this many unpacked bytes, refuse a ratio beyond maxCompressionRatio.
    std::uint64_t ratioCheckFrom = 64ull * 1024 * 1024;
    double maxCompressionRatio = 1000.0;
};

// Checks one entry name and returns it normalised: "dir/file.txt", directories without the
// trailing slash. Refuses everything listed above.
Result<std::string> normalizeEntryPath(std::string_view name, const ExtractionLimits& limits = {});

enum class EntryType { File, Directory };

struct ArchiveEntry {
    std::string path;          // normalised
    EntryType type = EntryType::File;
    std::uint64_t size = 0;    // as declared in the archive (checked while extracting)
};

struct ArchiveListing {
    std::string formatName;    // as libarchive names it, e.g. "ZIP 2.0 (deflation)"
    std::vector<ArchiveEntry> entries;
    std::size_t fileCount = 0;
    std::uint64_t totalBytes = 0;  // declared sizes of all files
    std::uint64_t archiveBytes = 0;
};

struct ExtractedFile {
    std::string path;          // relative to the extraction root
    std::uint64_t size = 0;
    std::string sha256;
    std::string head;          // first bytes, for file-type detection
};

struct ExtractedTree {
    std::filesystem::path root;
    ArchiveListing listing;
    std::vector<ExtractedFile> files;
};

class SecureExtractor {
public:
    explicit SecureExtractor(const security::SafeFs& fs, ExtractionLimits limits = {}) : fs_(fs), limits_(limits) {}

    // Reads only the entry headers and checks every entry.
    Result<ArchiveListing> inspect(const std::filesystem::path& archive, mods::ArchiveFormat format,
                                   const CancellationToken* cancel = nullptr) const;

    // Inspects, then extracts into `destination`, which must not exist yet. On any failure the
    // destination is removed again. `progress` receives (bytes written, bytes expected).
    Result<ExtractedTree> extract(const std::filesystem::path& archive, mods::ArchiveFormat format,
                                  const std::filesystem::path& destination, const CancellationToken* cancel = nullptr,
                                  const std::function<void(std::uint64_t, std::uint64_t)>& progress = {}) const;

    const ExtractionLimits& limits() const { return limits_; }

private:
    Result<ExtractedTree> extractInto(const std::filesystem::path& archive, mods::ArchiveFormat format,
                                      const std::filesystem::path& destination, const ArchiveListing& listing,
                                      const CancellationToken* cancel,
                                      const std::function<void(std::uint64_t, std::uint64_t)>& progress) const;
    Status verifyTree(const ExtractedTree& tree) const;

    const security::SafeFs& fs_;
    ExtractionLimits limits_;
};

// Number of bytes kept from the start of each file for type detection.
inline constexpr std::size_t kHeadBytes = 64;

}  // namespace akeno::archives
