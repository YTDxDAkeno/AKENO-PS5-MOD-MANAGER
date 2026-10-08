// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only parsers for the Unreal Engine container formats a mod archive can contain:
//
//   .pak   footer (versions 1-11) and index: mount point, entry count, file names, SHA-1 checks
//   .utoc  IoStore table of contents (versions 1-8): container id and flags, chunk ids,
//          compression blocks, compression methods, directory index, chunk hashes
//   .ucas  chunk data, only when uncompressed and unencrypted
//   container header chunk: the package ids a container provides
//   Zen package header (UE 5.0-5.2 and 5.3+ layouts): package name, flags, imported packages
//   legacy .uasset summary: tag, file versions, unversioned or not
//
// Every read is bounded and checked. Nothing here guesses: a field that cannot be read with
// certainty makes the result Unsupported or Malformed, and callers report the compatibility as
// unknown. Nothing is decrypted or decompressed (Oodle is not available on the console).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Result.hpp"

namespace akeno::unreal {

// Random access to one file. Implementations bound every read.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const = 0;
    // Exactly `length` bytes at `offset`; an error when the range is not inside the file.
    virtual Result<std::string> read(std::uint64_t offset, std::size_t length) const = 0;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::string data) : data_(std::move(data)) {}
    std::uint64_t size() const override { return data_.size(); }
    Result<std::string> read(std::uint64_t offset, std::size_t length) const override;

private:
    std::string data_;
};

// A regular file opened read-only from a descriptor the caller obtained without following links.
class FileSource final : public ByteSource {
public:
    // Takes ownership of `fd`; refuses anything that is not a regular file.
    static Result<std::unique_ptr<FileSource>> fromDescriptor(int fd, std::string label);
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    std::uint64_t size() const override { return size_; }
    Result<std::string> read(std::uint64_t offset, std::size_t length) const override;

private:
    FileSource(int fd, std::uint64_t size, std::string label) : fd_(fd), size_(size), label_(std::move(label)) {}
    int fd_ = -1;
    std::uint64_t size_ = 0;
    std::string label_;
};

enum class ParseStatus { Parsed, Unsupported, Malformed };
std::string_view toString(ParseStatus status) noexcept;

// ---------------------------------------------------------------------------------------- pak

inline constexpr std::uint32_t kPakMagic = 0x5A6F12E1;

struct PakFile {
    ParseStatus status = ParseStatus::Malformed;
    std::string detail;                     // why it is not Parsed, or a limitation
    int version = 0;
    bool encryptedIndex = false;
    std::string encryptionKeyGuid;          // hex; empty when none
    std::vector<std::string> compressionMethods;
    std::uint64_t indexOffset = 0;
    std::uint64_t indexSize = 0;
    bool indexHashVerified = false;         // SHA-1 of the index equals the footer's hash
    bool secondaryIndexesVerified = false;  // version 10+: path-hash and directory index hashes
    std::string mountPoint;
    std::int64_t entryCount = -1;           // -1 when unknown (encrypted index)
    std::vector<std::string> files;         // mount point + name, at most kMaxListedFiles
    bool filesComplete = false;
};
inline constexpr std::size_t kMaxListedFiles = 20000;
// `footerOnly`: version, flags and compression methods without reading the index (game files).
PakFile parsePak(const ByteSource& source, bool footerOnly = false);

// --------------------------------------------------------------------------------- IoStore

enum class IoChunkType : std::uint8_t {
    Invalid = 0,
    ExportBundleData = 1,
    BulkData = 2,
    OptionalBulkData = 3,
    MemoryMappedBulkData = 4,
    ScriptObjects = 5,
    ContainerHeader = 6,
    ExternalFile = 7,
    ShaderCodeLibrary = 8,
    ShaderCode = 9,
    PackageStoreEntry = 10,
    DerivedData = 11,
    EditorDerivedData = 12,
    PackageResource = 13,
};
std::string chunkTypeName(std::uint8_t type);

struct IoChunk {
    std::uint64_t id = 0;       // package id for package chunks
    std::uint16_t index = 0;
    std::uint8_t type = 0;
    std::uint64_t offset = 0;   // in the uncompressed address space of the container
    std::uint64_t length = 0;
    std::string hash;           // hex of the stored chunk hash (20 or 32 bytes)
    std::uint8_t metaFlags = 0;
};

struct IoBlock {
    std::uint64_t offset = 0;   // in the .ucas file(s)
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint8_t method = 0;    // 0 = none, else 1-based into compressionMethods
};

inline constexpr char kTocMagic[] = "-==--==--==--==-";
inline constexpr int kLatestTocVersion = 8;  // ReplaceIoChunkHashWithIoHash

struct IoStoreToc {
    ParseStatus status = ParseStatus::Malformed;
    std::string detail;
    int version = 0;
    std::uint64_t containerId = 0;
    std::uint8_t flags = 0;
    std::string encryptionKeyGuid;           // hex; empty when none
    std::uint32_t compressionBlockSize = 0;
    std::vector<std::string> compressionMethods;
    std::uint64_t partitionCount = 1;
    std::uint64_t partitionSize = 0;
    std::uint64_t entryCount = 0;            // from the header, also when chunks are not loaded
    std::vector<IoChunk> chunks;             // empty for a chunk-id-only parse
    std::vector<std::uint64_t> packageIds;   // chunk-id-only parse: ExportBundleData ids, sorted
    std::vector<IoBlock> blocks;
    std::string mountPoint;
    std::vector<std::pair<std::string, std::uint32_t>> files;  // directory index: path, chunk index
    bool directoryIndexParsed = false;
    std::string directoryIndexDetail;
    std::uint64_t casBytesRequired = 0;      // end of the last block in the first partition

    bool compressed() const { return (flags & 0x01) != 0; }
    bool encrypted() const { return (flags & 0x02) != 0; }
    bool signedContainer() const { return (flags & 0x04) != 0; }
    bool indexed() const { return (flags & 0x08) != 0; }
};

struct TocParseOptions {
    bool chunkIdsOnly = false;               // header and chunk ids (for a game's large containers)
    std::uint64_t maxTocBytes = 64ull * 1024 * 1024;   // whole-table parse (mods)
    std::uint64_t maxEntries = 4u * 1024 * 1024;
};
IoStoreToc parseIoStoreToc(const ByteSource& toc, const TocParseOptions& options = {});

// One chunk's bytes from the .ucas: only uncompressed, unencrypted, single-partition containers.
Result<std::string> readChunk(const IoStoreToc& toc, std::size_t chunk, const ByteSource& cas, std::uint64_t maxBytes);

struct ChunkVerification {
    std::size_t verified = 0;
    std::size_t mismatched = 0;
    std::size_t skipped = 0;
    std::string algorithm;                   // "BLAKE3-160", "SHA-1" or both for old versions
    std::vector<std::string> notes;
    bool allVerified() const { return verified > 0 && mismatched == 0 && skipped == 0; }
};
// Recomputes chunk hashes from the .ucas data within `maxBytes` of reading.
ChunkVerification verifyChunks(const IoStoreToc& toc, const ByteSource& cas, std::uint64_t maxBytes);

inline constexpr std::uint32_t kContainerHeaderSignature = 0x496f436e;  // "nCoI"

struct ContainerHeader {
    ParseStatus status = ParseStatus::Malformed;
    std::string detail;
    bool hasSignature = false;
    std::uint32_t version = 0;
    std::uint64_t containerId = 0;
    std::vector<std::uint64_t> packageIds;
};
ContainerHeader parseContainerHeader(std::string_view data);

// ------------------------------------------------------------------------------- packages

inline constexpr std::uint32_t kPkgCooked = 0x00000200;
inline constexpr std::uint32_t kPkgUnversionedProperties = 0x00002000;
inline constexpr std::uint32_t kPkgFilterEditorOnly = 0x80000000;

struct ZenPackage {
    ParseStatus status = ParseStatus::Malformed;
    std::string detail;
    std::string layout;                      // "UE 5.3+" or "UE 5.0-5.2"
    bool hasVersioningInfo = false;
    std::uint32_t headerSize = 0;
    std::uint32_t packageFlags = 0;
    std::string name;                        // "/Game/Folder/Asset"
    std::size_t nameCount = 0;
    std::vector<std::string> importedPackages;
    bool importsParsed = false;

    bool cooked() const { return (packageFlags & kPkgCooked) != 0; }
    bool unversioned() const { return (packageFlags & kPkgUnversionedProperties) != 0; }
};
ZenPackage parseZenPackage(std::string_view header);

inline constexpr std::uint32_t kPackageFileTag = 0x9E2A83C1;

struct LegacyPackageSummary {
    ParseStatus status = ParseStatus::Malformed;
    std::string detail;
    int legacyFileVersion = 0;
    int fileVersionUE4 = 0;
    int fileVersionUE5 = 0;
    int licenseeVersion = 0;
    bool unversioned() const { return fileVersionUE4 == 0 && fileVersionUE5 == 0 && licenseeVersion == 0; }
};
LegacyPackageSummary parseLegacyPackageSummary(std::string_view head);

// "../../../Game/Content/X.uasset" -> "Game/Content/X.uasset"; "/Game/X" stays.
std::string stripMountPrefix(std::string_view path);

}  // namespace akeno::unreal
