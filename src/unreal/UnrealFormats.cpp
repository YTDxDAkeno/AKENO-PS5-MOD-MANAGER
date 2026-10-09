// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/unreal/UnrealFormats.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

#include "akeno/core/Strings.hpp"
#include "akeno/security/Digests.hpp"

namespace akeno::unreal {

namespace {

constexpr std::size_t kMaxStringChars = 4096;
constexpr std::uint64_t kMaxIndexBytes = 64ull * 1024 * 1024;

// Little-endian reader over a byte range; any read past the end clears ok() for good.
class Cursor {
public:
    explicit Cursor(std::string_view data, std::size_t position = 0) : data_(data), pos_(position) {
        if (pos_ > data_.size()) ok_ = false;
    }
    bool ok() const { return ok_; }
    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return ok_ ? data_.size() - pos_ : 0; }

    std::uint64_t unsignedLe(int bytes) {
        if (!need(static_cast<std::size_t>(bytes))) return 0;
        std::uint64_t value = 0;
        for (int i = bytes - 1; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(data_[pos_ + i]);
        pos_ += static_cast<std::size_t>(bytes);
        return value;
    }
    std::uint64_t unsignedBe(int bytes) {
        if (!need(static_cast<std::size_t>(bytes))) return 0;
        std::uint64_t value = 0;
        for (int i = 0; i < bytes; ++i) value = (value << 8) | static_cast<unsigned char>(data_[pos_ + i]);
        pos_ += static_cast<std::size_t>(bytes);
        return value;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(unsignedLe(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(unsignedLe(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(unsignedLe(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return unsignedLe(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    std::string_view bytes(std::size_t count) {
        if (!need(count)) return {};
        std::string_view view = data_.substr(pos_, count);
        pos_ += count;
        return view;
    }
    bool skip(std::uint64_t count) {
        if (count > remaining()) {
            ok_ = false;
            return false;
        }
        pos_ += static_cast<std::size_t>(count);
        return true;
    }
    // FString: int32 length including the terminator; negative for UTF-16.
    bool fstring(std::string& out) {
        const std::int32_t length = i32();
        if (!ok_) return false;
        if (length == 0) {
            out.clear();
            return true;
        }
        if (length > 0) {
            if (static_cast<std::size_t>(length) > kMaxStringChars) return fail();
            std::string_view raw = bytes(static_cast<std::size_t>(length));
            if (!ok_ || raw.back() != '\0') return fail();
            out = latin1ToUtf8(raw.substr(0, raw.size() - 1));
            return true;
        }
        if (length == INT32_MIN || static_cast<std::size_t>(-static_cast<std::int64_t>(length)) > kMaxStringChars) {
            return fail();
        }
        const auto units = static_cast<std::size_t>(-static_cast<std::int64_t>(length));
        std::string_view raw = bytes(units * 2);
        if (!ok_ || raw[raw.size() - 1] != '\0' || raw[raw.size() - 2] != '\0') return fail();
        out = utf16ToUtf8(raw.substr(0, raw.size() - 2));
        return true;
    }
    bool fail() {
        ok_ = false;
        return false;
    }

    static std::string latin1ToUtf8(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x80) {
                out.push_back(c);
            } else {
                out.push_back(static_cast<char>(0xC0 | (byte >> 6)));
                out.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
            }
        }
        return out;
    }
    static std::string utf16ToUtf8(std::string_view raw) {
        std::string out;
        for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            std::uint32_t unit = static_cast<unsigned char>(raw[i]) | (static_cast<unsigned char>(raw[i + 1]) << 8);
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < raw.size()) {
                const std::uint32_t low = static_cast<unsigned char>(raw[i + 2]) | (static_cast<unsigned char>(raw[i + 3]) << 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                } else {
                    unit = '?';
                }
            } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                unit = '?';
            }
            if (unit < 0x80) {
                out.push_back(static_cast<char>(unit));
            } else if (unit < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
                out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            } else if (unit < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
                out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (unit >> 18)));
                out.push_back(static_cast<char>(0x80 | ((unit >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            }
        }
        return out;
    }

private:
    bool need(std::size_t count) {
        if (!ok_ || count > data_.size() - pos_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::string_view data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::string hexOf(std::string_view raw) {
    return security::toHex(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
}

std::string guidOf(std::string_view raw) {
    const bool zero = std::all_of(raw.begin(), raw.end(), [](char c) { return c == '\0'; });
    return zero ? std::string() : hexOf(raw);
}

std::string fixedName(std::string_view raw) {
    std::string name(raw.substr(0, raw.find('\0')));
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) return "?";
    }
    return name;
}

bool sha1Matches(std::string_view data, std::string_view expected) {
    const auto digest = security::sha1(data);
    return expected.size() == digest.size() && std::memcmp(digest.data(), expected.data(), digest.size()) == 0;
}

template <typename T>
T fail(ParseStatus status, std::string detail) {
    T result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

}  // namespace

std::string_view toString(ParseStatus status) noexcept {
    switch (status) {
        case ParseStatus::Parsed: return "parsed";
        case ParseStatus::Unsupported: return "unsupported";
        case ParseStatus::Malformed: return "malformed";
    }
    return "malformed";
}

Result<std::string> MemorySource::read(std::uint64_t offset, std::size_t length) const {
    if (offset > data_.size() || length > data_.size() - offset) {
        return makeError(ErrorCode::ParseError, "Read outside the data.", strings::concat(offset, "+", length));
    }
    return data_.substr(static_cast<std::size_t>(offset), length);
}

Result<std::unique_ptr<FileSource>> FileSource::fromDescriptor(int fd, std::string label) {
    struct stat info {};
    if (fd < 0 || ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        if (fd >= 0) ::close(fd);
        return makeError(ErrorCode::IoError, "Not a readable regular file.", label);
    }
    return std::unique_ptr<FileSource>(new FileSource(fd, static_cast<std::uint64_t>(info.st_size), std::move(label)));
}

FileSource::~FileSource() {
    if (fd_ >= 0) ::close(fd_);
}

Result<std::string> FileSource::read(std::uint64_t offset, std::size_t length) const {
    if (offset > size_ || length > size_ - offset) {
        return makeError(ErrorCode::ParseError, "Read outside the file.", label_);
    }
    std::string buffer(length, '\0');
    std::size_t done = 0;
    while (done < length) {
        const ssize_t n = ::pread(fd_, buffer.data() + done, length - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return makeError(ErrorCode::IoError, "Could not read a file.", label_);
        done += static_cast<std::size_t>(n);
    }
    return buffer;
}

std::string stripMountPrefix(std::string_view path) {
    while (strings::startsWith(path, "../")) path.remove_prefix(3);
    return std::string(path);
}

// ---------------------------------------------------------------------------------------- pak

namespace {

struct FooterLayout {
    std::size_t size;
    bool guid;
    int methods;
    bool frozen;
    int minVersion;
    int maxVersion;
};

// FPakInfo::GetSerializedSize for each version family (UE 4.22 wrote four method slots).
constexpr FooterLayout kFooters[] = {
    {221, true, 5, false, 10, 11}, {222, true, 5, true, 9, 9}, {221, true, 5, false, 8, 8},
    {189, true, 4, false, 8, 8},   {61, true, 0, false, 7, 7}, {45, false, 0, false, 1, 6},
};

bool parseIndexV10(const ByteSource& source, Cursor& c, PakFile& pak) {
    (void)c.u64();  // path hash seed
    const std::uint32_t hasPathHashIndex = c.u32();
    std::int64_t phiOffset = 0, phiSize = 0, fdiOffset = 0, fdiSize = 0;
    std::string_view phiHash, fdiHash;
    if (hasPathHashIndex > 1) return c.fail();
    if (hasPathHashIndex == 1) {
        phiOffset = c.i64();
        phiSize = c.i64();
        phiHash = c.bytes(20);
    }
    const std::uint32_t hasFullDirectoryIndex = c.u32();
    if (hasFullDirectoryIndex > 1) return c.fail();
    if (hasFullDirectoryIndex == 1) {
        fdiOffset = c.i64();
        fdiSize = c.i64();
        fdiHash = c.bytes(20);
    }
    const std::int32_t encodedSize = c.i32();
    if (!c.ok() || encodedSize < 0 || !c.skip(static_cast<std::uint64_t>(encodedSize))) return c.fail();
    const std::int32_t unencoded = c.i32();
    if (!c.ok() || unencoded < 0) return c.fail();

    auto region = [&](std::int64_t offset, std::int64_t size, std::string_view hash) -> std::optional<std::string> {
        if (offset < 0 || size < 0 || static_cast<std::uint64_t>(size) > kMaxIndexBytes ||
            static_cast<std::uint64_t>(offset) > source.size() ||
            static_cast<std::uint64_t>(size) > source.size() - static_cast<std::uint64_t>(offset)) {
            return std::nullopt;
        }
        auto data = source.read(static_cast<std::uint64_t>(offset), static_cast<std::size_t>(size));
        if (!data || !sha1Matches(data.value(), hash)) return std::nullopt;
        return std::move(data).value();
    };
    bool secondaryOk = true;
    if (hasPathHashIndex == 1 && !region(phiOffset, phiSize, phiHash)) secondaryOk = false;
    if (hasFullDirectoryIndex == 1) {
        auto directory = region(fdiOffset, fdiSize, fdiHash);
        if (!directory) {
            secondaryOk = false;
        } else {
            Cursor d(*directory);
            const std::int32_t directories = d.i32();
            bool complete = d.ok() && directories >= 0;
            for (std::int32_t i = 0; complete && i < directories; ++i) {
                std::string dir;
                if (!d.fstring(dir)) break;
                const std::int32_t count = d.i32();
                if (!d.ok() || count < 0) break;
                for (std::int32_t j = 0; j < count; ++j) {
                    std::string name;
                    if (!d.fstring(name)) break;
                    (void)d.i32();
                    if (pak.files.size() >= kMaxListedFiles) {
                        complete = false;
                        break;
                    }
                    pak.files.push_back(pak.mountPoint + dir + name);
                }
            }
            pak.filesComplete = complete && d.ok() && d.remaining() == 0;
        }
    } else {
        pak.filesComplete = pak.entryCount == 0;
    }
    pak.secondaryIndexesVerified = secondaryOk;
    return true;
}

bool parseIndexLegacy(Cursor& c, PakFile& pak) {
    const auto count = static_cast<std::size_t>(pak.entryCount);
    for (std::size_t i = 0; i < count; ++i) {
        std::string name;
        if (!c.fstring(name)) return false;
        (void)c.i64();  // offset
        (void)c.i64();  // size
        (void)c.i64();  // uncompressed size
        const std::uint32_t method = c.u32();
        if (pak.version <= 1) (void)c.u64();  // timestamp
        (void)c.bytes(20);
        if (pak.version >= 3) {
            if (method != 0) {
                const std::int32_t blocks = c.i32();
                if (!c.ok() || blocks < 0 || !c.skip(static_cast<std::uint64_t>(blocks) * 16)) return c.fail();
            }
            (void)c.u8();   // flags (encrypted, deleted)
            (void)c.u32();  // compression block size
        }
        if (!c.ok()) return false;
        if (pak.files.size() < kMaxListedFiles) pak.files.push_back(pak.mountPoint + name);
    }
    pak.filesComplete = pak.files.size() == count;
    return true;
}

}  // namespace

PakFile parsePak(const ByteSource& source, bool footerOnly) {
    const std::uint64_t size = source.size();
    if (size < 45) return fail<PakFile>(ParseStatus::Malformed, "Too small to be a .pak file.");
    PakFile pak;
    bool found = false;
    std::optional<int> unknownVersion;
    for (const auto& layout : kFooters) {
        if (size < layout.size) continue;
        auto footer = source.read(size - layout.size, layout.size);
        if (!footer) continue;
        Cursor c(footer.value());
        std::string_view guid;
        if (layout.guid) guid = c.bytes(16);
        const std::uint8_t encrypted = c.u8();
        if (c.u32() != kPakMagic) continue;
        const std::int32_t version = c.i32();
        if (version < layout.minVersion || version > layout.maxVersion) {
            if (version > 11) unknownVersion = version;
            continue;
        }
        const std::int64_t indexOffset = c.i64();
        const std::int64_t indexSize = c.i64();
        const std::string_view hash = c.bytes(20);
        if (layout.frozen) (void)c.u8();
        std::vector<std::string> methods;
        for (int i = 0; i < layout.methods; ++i) {
            std::string name = fixedName(c.bytes(32));
            if (!name.empty()) methods.push_back(std::move(name));
        }
        // Footers before version 4 had no encrypted-index byte: the byte read above belongs to
        // the index hash and is ignored.
        if (!c.ok() || (version >= 4 && encrypted > 1) || indexOffset < 0 || indexSize < 0 ||
            static_cast<std::uint64_t>(indexOffset) > size - layout.size ||
            static_cast<std::uint64_t>(indexSize) > size - layout.size - static_cast<std::uint64_t>(indexOffset)) {
            return fail<PakFile>(ParseStatus::Malformed, "The .pak footer points outside the file.");
        }
        pak.version = version;
        pak.encryptedIndex = encrypted == 1 && version >= 4;
        pak.encryptionKeyGuid = layout.guid ? guidOf(guid) : std::string();
        pak.compressionMethods = std::move(methods);
        pak.indexOffset = static_cast<std::uint64_t>(indexOffset);
        pak.indexSize = static_cast<std::uint64_t>(indexSize);
        found = true;
        if (footerOnly) {
            pak.status = ParseStatus::Parsed;
            pak.detail = "Footer only.";
            return pak;
        }
        if (pak.encryptedIndex) {
            pak.status = ParseStatus::Parsed;
            pak.detail = "The index is encrypted; file names and counts cannot be read without the game's key.";
            return pak;
        }
        if (pak.indexSize > kMaxIndexBytes) {
            pak.status = ParseStatus::Unsupported;
            pak.detail = "The index is too large to inspect.";
            return pak;
        }
        auto index = source.read(pak.indexOffset, static_cast<std::size_t>(pak.indexSize));
        if (!index) return fail<PakFile>(ParseStatus::Malformed, "Unreadable .pak index.");
        pak.indexHashVerified = sha1Matches(index.value(), hash);
        if (!pak.indexHashVerified) {
            pak.status = ParseStatus::Malformed;
            pak.detail = "The .pak index does not match its SHA-1 hash (damaged or modified).";
            return pak;
        }
        Cursor c2(index.value());
        if (!c2.fstring(pak.mountPoint)) return fail<PakFile>(ParseStatus::Malformed, "Unreadable mount point.");
        const std::int32_t entries = c2.i32();
        if (!c2.ok() || entries < 0 || entries > 10'000'000) {
            return fail<PakFile>(ParseStatus::Malformed, "Unreadable entry count.");
        }
        pak.entryCount = entries;
        const bool parsed = version >= 10 ? parseIndexV10(source, c2, pak) : parseIndexLegacy(c2, pak);
        if (!parsed) {
            pak.status = ParseStatus::Malformed;
            pak.detail = "The .pak index could not be read completely.";
            return pak;
        }
        pak.status = ParseStatus::Parsed;
        if (version >= 10 && !pak.secondaryIndexesVerified) {
            pak.status = ParseStatus::Malformed;
            pak.detail = "A secondary .pak index does not match its SHA-1 hash.";
        }
        return pak;
    }
    if (!found && unknownVersion) {
        return fail<PakFile>(ParseStatus::Unsupported, strings::concat(".pak version ", *unknownVersion, " is newer than Akeno knows (11)."));
    }
    return fail<PakFile>(ParseStatus::Malformed, "No .pak footer was found.");
}

// --------------------------------------------------------------------------------- IoStore

std::string chunkTypeName(std::uint8_t type) {
    static constexpr const char* kNames[] = {"Invalid",           "ExportBundleData", "BulkData",
                                             "OptionalBulkData",  "MemoryMappedBulkData", "ScriptObjects",
                                             "ContainerHeader",   "ExternalFile",     "ShaderCodeLibrary",
                                             "ShaderCode",        "PackageStoreEntry", "DerivedData",
                                             "EditorDerivedData", "PackageResource"};
    if (type < std::size(kNames)) return kNames[type];
    return strings::concat("type ", static_cast<int>(type));
}

IoStoreToc parseIoStoreToc(const ByteSource& source, const TocParseOptions& options) {
    constexpr std::size_t kHeaderSize = 144;
    if (source.size() < kHeaderSize) return fail<IoStoreToc>(ParseStatus::Malformed, "Too small to be a .utoc file.");
    auto headerBytes = source.read(0, kHeaderSize);
    if (!headerBytes) return fail<IoStoreToc>(ParseStatus::Malformed, "Unreadable header.");
    Cursor h(headerBytes.value());
    IoStoreToc toc;
    if (h.bytes(16) != std::string_view(kTocMagic, 16)) {
        return fail<IoStoreToc>(ParseStatus::Malformed, "No IoStore table-of-contents signature.");
    }
    toc.version = h.u8();
    (void)h.u8();
    (void)h.u16();
    const std::uint32_t headerSize = h.u32();
    const std::uint32_t entryCount = h.u32();
    const std::uint32_t blockCount = h.u32();
    const std::uint32_t blockEntrySize = h.u32();
    const std::uint32_t methodCount = h.u32();
    const std::uint32_t methodLength = h.u32();
    toc.compressionBlockSize = h.u32();
    const std::uint32_t directoryIndexSize = h.u32();
    const std::uint32_t partitionCount = h.u32();
    toc.containerId = h.u64();
    toc.encryptionKeyGuid = guidOf(h.bytes(16));
    toc.flags = h.u8();
    (void)h.u8();
    (void)h.u16();
    const std::uint32_t seedsCount = h.u32();
    const std::uint64_t partitionSize = h.u64();
    const std::uint32_t overflowCount = h.u32();
    toc.entryCount = entryCount;
    if (toc.version < 1) return fail<IoStoreToc>(ParseStatus::Malformed, "Invalid IoStore version 0.");
    if (toc.version > kLatestTocVersion) {
        IoStoreToc unsupported = fail<IoStoreToc>(
            ParseStatus::Unsupported,
            strings::concat("IoStore version ", toc.version, " is newer than Akeno knows (", kLatestTocVersion, ")."));
        unsupported.version = toc.version;
        unsupported.containerId = toc.containerId;
        unsupported.flags = toc.flags;
        return unsupported;
    }
    if (headerSize != kHeaderSize || blockEntrySize != 12 || entryCount > options.maxEntries ||
        methodLength > 256 || methodCount > 64 || (blockCount > 0 && toc.compressionBlockSize == 0)) {
        return fail<IoStoreToc>(ParseStatus::Malformed, "Inconsistent IoStore header.");
    }
    if (toc.version >= 3) {
        toc.partitionCount = partitionCount == 0 ? 1 : partitionCount;
        toc.partitionSize = partitionSize;
    }
    if (options.headerOnly) {
        toc.status = ParseStatus::Parsed;
        toc.detail = "Header only.";
        return toc;
    }
    if (source.size() > options.maxTocBytes && !options.chunkIdsOnly) {
        toc.status = ParseStatus::Unsupported;
        toc.detail = "The table of contents is too large to inspect completely.";
        return toc;
    }

    // Chunk ids follow the header directly; that is all a chunk-id-only parse needs.
    const std::uint64_t idsBytes = static_cast<std::uint64_t>(entryCount) * 12;
    if (kHeaderSize + idsBytes > source.size()) return fail<IoStoreToc>(ParseStatus::Malformed, "Truncated chunk table.");
    auto ids = source.read(kHeaderSize, static_cast<std::size_t>(idsBytes));
    if (!ids) return fail<IoStoreToc>(ParseStatus::Malformed, "Unreadable chunk table.");
    Cursor ic(ids.value());
    if (options.chunkIdsOnly) {
        // Only package ids are kept: a game's table can list millions of chunks.
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            const std::uint64_t id = ic.u64();
            (void)ic.unsignedBe(2);
            (void)ic.u8();
            if (ic.u8() == static_cast<std::uint8_t>(IoChunkType::ExportBundleData)) toc.packageIds.push_back(id);
        }
        std::sort(toc.packageIds.begin(), toc.packageIds.end());
        // Compression method names sit behind the offset and block tables; read only them.
        std::uint64_t methodsAt = kHeaderSize + idsBytes + static_cast<std::uint64_t>(entryCount) * 10 +
                                  static_cast<std::uint64_t>(blockCount) * 12;
        if (toc.version >= 4) methodsAt += static_cast<std::uint64_t>(seedsCount) * 4;
        if (toc.version >= 5) methodsAt += static_cast<std::uint64_t>(overflowCount) * 4;
        const std::uint64_t methodsBytes = static_cast<std::uint64_t>(methodCount) * methodLength;
        if (methodsAt <= source.size() && methodsBytes <= source.size() - methodsAt) {
            if (auto names = source.read(methodsAt, static_cast<std::size_t>(methodsBytes)); names) {
                for (std::uint32_t i = 0; i < methodCount; ++i) {
                    toc.compressionMethods.push_back(fixedName(std::string_view(names.value()).substr(i * methodLength, methodLength)));
                }
            }
        }
        toc.status = ParseStatus::Parsed;
        toc.detail = "Header and chunk ids only.";
        return toc;
    }
    // Every chunk needs an id, an offset/length pair and metadata: refuse a count the file
    // cannot hold before allocating anything for it.
    const std::uint64_t perChunk = 12 + 10 + (toc.version >= 8 ? 24 : 33);
    if (kHeaderSize + static_cast<std::uint64_t>(entryCount) * perChunk > source.size()) {
        return fail<IoStoreToc>(ParseStatus::Malformed, "The chunk count does not fit the table of contents.");
    }
    toc.chunks.resize(entryCount);
    for (auto& chunk : toc.chunks) {
        chunk.id = ic.u64();
        chunk.index = static_cast<std::uint16_t>(ic.unsignedBe(2));
        (void)ic.u8();
        chunk.type = ic.u8();
    }

    auto all = source.read(0, static_cast<std::size_t>(source.size()));
    if (!all) return fail<IoStoreToc>(ParseStatus::Malformed, "Unreadable table of contents.");
    Cursor c(all.value(), static_cast<std::size_t>(kHeaderSize + idsBytes));
    for (auto& chunk : toc.chunks) {
        chunk.offset = c.unsignedBe(5);
        chunk.length = c.unsignedBe(5);
    }
    if (toc.version >= 4) c.skip(static_cast<std::uint64_t>(seedsCount) * 4);
    if (toc.version >= 5) c.skip(static_cast<std::uint64_t>(overflowCount) * 4);
    if (!c.ok() || static_cast<std::uint64_t>(blockCount) * 12 > c.remaining()) {
        return fail<IoStoreToc>(ParseStatus::Malformed, "Truncated compression block table.");
    }
    toc.blocks.resize(blockCount);
    for (auto& block : toc.blocks) {
        block.offset = c.unsignedLe(5);
        block.compressedSize = static_cast<std::uint32_t>(c.unsignedLe(3));
        block.uncompressedSize = static_cast<std::uint32_t>(c.unsignedLe(3));
        block.method = c.u8();
    }
    for (std::uint32_t i = 0; i < methodCount; ++i) toc.compressionMethods.push_back(fixedName(c.bytes(methodLength)));
    if (toc.signedContainer()) {
        const std::int32_t hashSize = c.i32();
        if (!c.ok() || hashSize < 0 || hashSize > 4096) return fail<IoStoreToc>(ParseStatus::Malformed, "Invalid signature size.");
        c.skip(static_cast<std::uint64_t>(hashSize) * 2 + static_cast<std::uint64_t>(blockCount) * 20);
    }
    std::string_view directoryIndex;
    if (toc.version >= 2 && toc.indexed() && directoryIndexSize > 0) directoryIndex = c.bytes(directoryIndexSize);
    const std::size_t metaSize = toc.version >= 8 ? 24 : 33;
    for (auto& chunk : toc.chunks) {
        const std::string_view meta = c.bytes(metaSize);
        if (!c.ok()) break;
        chunk.hash = hexOf(meta.substr(0, toc.version >= 8 ? 20 : 32));
        chunk.metaFlags = static_cast<std::uint8_t>(meta[toc.version >= 8 ? 20 : 32]);
    }
    if (!c.ok()) return fail<IoStoreToc>(ParseStatus::Malformed, "Truncated chunk metadata.");
    if (c.remaining() != 0 && toc.version != 6) {
        return fail<IoStoreToc>(ParseStatus::Malformed,
                                strings::concat(c.remaining(), " unexpected bytes after the table of contents."));
    }

    // Consistency: every chunk inside the block table, every block method known.
    const std::uint64_t virtualSize = static_cast<std::uint64_t>(blockCount) * toc.compressionBlockSize;
    for (const auto& chunk : toc.chunks) {
        if (chunk.length > 0 && (chunk.offset > virtualSize || chunk.length > virtualSize - chunk.offset)) {
            return fail<IoStoreToc>(ParseStatus::Malformed, "A chunk lies outside the compression blocks.");
        }
    }
    for (const auto& block : toc.blocks) {
        if (block.method > toc.compressionMethods.size() || block.uncompressedSize > toc.compressionBlockSize) {
            return fail<IoStoreToc>(ParseStatus::Malformed, "A compression block is inconsistent.");
        }
        const std::uint64_t partition = toc.partitionSize > 0 && toc.partitionCount > 1 ? block.offset / toc.partitionSize : 0;
        if (partition == 0) {
            toc.casBytesRequired = std::max<std::uint64_t>(toc.casBytesRequired, block.offset + block.compressedSize);
        }
    }

    // Directory index: mount point, directories, files, string table.
    if (!directoryIndex.empty()) {
        if (toc.encrypted()) {
            toc.directoryIndexDetail = "The directory index is encrypted.";
        } else {
            Cursor d(directoryIndex);
            std::string mount;
            d.fstring(mount);
            struct Dir { std::uint32_t name, firstChild, nextSibling, firstFile; };
            struct File { std::uint32_t name, next, userData; };
            std::vector<Dir> dirs;
            std::vector<File> files;
            std::vector<std::string> strings;
            const std::int32_t dirCount = d.i32();
            if (d.ok() && dirCount >= 0 && static_cast<std::uint64_t>(dirCount) * 16 <= d.remaining()) {
                dirs.resize(static_cast<std::size_t>(dirCount));
                for (auto& dir : dirs) dir = {d.u32(), d.u32(), d.u32(), d.u32()};
            } else {
                d.fail();
            }
            const std::int32_t fileCount = d.i32();
            if (d.ok() && fileCount >= 0 && static_cast<std::uint64_t>(fileCount) * 12 <= d.remaining()) {
                files.resize(static_cast<std::size_t>(fileCount));
                for (auto& file : files) file = {d.u32(), d.u32(), d.u32()};
            } else {
                d.fail();
            }
            const std::int32_t stringCount = d.i32();
            if (d.ok() && stringCount >= 0 && static_cast<std::uint64_t>(stringCount) * 4 <= d.remaining()) {
                strings.resize(static_cast<std::size_t>(stringCount));
                for (auto& text : strings) d.fstring(text);
            } else {
                d.fail();
            }
            constexpr std::uint32_t kNone = 0xFFFFFFFFu;
            bool consistent = d.ok() && d.remaining() == 0 && !dirs.empty();
            auto nameOf = [&](std::uint32_t index) -> std::optional<std::string> {
                if (index == kNone) return std::string();
                if (index >= strings.size()) return std::nullopt;
                return strings[index];
            };
            std::set<std::uint32_t> visited;
            std::function<void(std::uint32_t, const std::string&, int)> visit = [&](std::uint32_t index, const std::string& prefix, int depth) {
                while (consistent && index != kNone) {
                    if (index >= dirs.size() || depth > 64 || !visited.insert(index).second) {
                        consistent = false;
                        return;
                    }
                    const Dir& dir = dirs[index];
                    auto name = nameOf(dir.name);
                    if (!name) {
                        consistent = false;
                        return;
                    }
                    const std::string path = name->empty() ? prefix : prefix + *name + "/";
                    std::set<std::uint32_t> seenFiles;
                    for (std::uint32_t f = dir.firstFile; consistent && f != kNone; f = files[f].next) {
                        if (f >= files.size() || !seenFiles.insert(f).second) {
                            consistent = false;
                            return;
                        }
                        auto fileName = nameOf(files[f].name);
                        if (!fileName || fileName->empty() || files[f].userData >= toc.chunks.size()) {
                            consistent = false;
                            return;
                        }
                        if (toc.files.size() < kMaxListedFiles) toc.files.emplace_back(path + *fileName, files[f].userData);
                    }
                    visit(dir.firstChild, path, depth + 1);
                    index = depth == 0 ? kNone : dir.nextSibling;
                }
            };
            if (consistent) visit(0, "", 0);
            toc.directoryIndexParsed = consistent;
            toc.mountPoint = mount;
            if (!consistent) {
                toc.files.clear();
                toc.directoryIndexDetail = "The directory index is inconsistent.";
            }
        }
    }
    toc.status = ParseStatus::Parsed;
    return toc;
}

Result<std::string> readChunk(const IoStoreToc& toc, std::size_t index, const ByteSource& cas, std::uint64_t maxBytes) {
    if (toc.status != ParseStatus::Parsed || index >= toc.chunks.size() || toc.blocks.empty()) {
        return makeError(ErrorCode::InvalidArgument, "No such chunk.");
    }
    if (toc.encrypted()) return makeError(ErrorCode::Unsupported, "The container is encrypted.");
    if (toc.partitionCount > 1) return makeError(ErrorCode::Unsupported, "Partitioned containers are not read.");
    const IoChunk& chunk = toc.chunks[index];
    if (chunk.length > maxBytes) return makeError(ErrorCode::ResponseTooLarge, "The chunk is larger than the read limit.");
    std::string data;
    data.reserve(static_cast<std::size_t>(chunk.length));
    const std::uint64_t blockSize = toc.compressionBlockSize;
    std::uint64_t position = chunk.offset;
    const std::uint64_t end = chunk.offset + chunk.length;
    while (position < end) {
        const std::uint64_t blockIndex = position / blockSize;
        if (blockIndex >= toc.blocks.size()) return makeError(ErrorCode::ParseError, "Chunk outside the blocks.");
        const IoBlock& block = toc.blocks[static_cast<std::size_t>(blockIndex)];
        if (block.method != 0) {
            const std::string method = block.method <= toc.compressionMethods.size() ? toc.compressionMethods[block.method - 1] : "?";
            return makeError(ErrorCode::Unsupported, "The chunk is compressed with " + method + ".");
        }
        if (block.compressedSize != block.uncompressedSize) return makeError(ErrorCode::ParseError, "Inconsistent block size.");
        const std::uint64_t inBlock = position - blockIndex * blockSize;
        if (inBlock >= block.uncompressedSize) return makeError(ErrorCode::ParseError, "Chunk outside its block.");
        const std::uint64_t take = std::min<std::uint64_t>(block.uncompressedSize - inBlock, end - position);
        auto bytes = cas.read(block.offset + inBlock, static_cast<std::size_t>(take));
        if (!bytes) return std::move(bytes).error();
        data += bytes.value();
        position += take;
    }
    return data;
}

ChunkVerification verifyChunks(const IoStoreToc& toc, const ByteSource& cas, std::uint64_t maxBytes) {
    ChunkVerification result;
    result.algorithm = toc.version >= 8 ? "BLAKE3-160" : "BLAKE3 or SHA-1";
    std::uint64_t budget = maxBytes;
    for (std::size_t i = 0; i < toc.chunks.size(); ++i) {
        const IoChunk& chunk = toc.chunks[i];
        if (chunk.length > budget) {
            ++result.skipped;
            continue;
        }
        auto data = readChunk(toc, i, cas, budget);
        if (!data) {
            ++result.skipped;
            if (result.notes.size() < 4) result.notes.push_back(chunkTypeName(chunk.type) + ": " + data.error().message);
            continue;
        }
        budget -= chunk.length;
        const std::string blake = security::toHex(security::blake3_160(data.value()));
        if (toc.version >= 8) {
            if (chunk.hash == blake) {
                ++result.verified;
            } else {
                ++result.mismatched;
            }
            continue;
        }
        // Older tables store 32 bytes: a full BLAKE3-256 digest, or a 20-byte digest (BLAKE3-160
        // or SHA-1, depending on the engine version) followed by zero padding.
        const bool padded = chunk.hash.size() == 64 && chunk.hash.substr(40) == std::string(24, '0');
        const std::string sha = security::toHex(security::sha1(data.value()));
        if (chunk.hash == security::toHex(security::blake3(data.value())) ||
            (padded && (chunk.hash.substr(0, 40) == blake || chunk.hash.substr(0, 40) == sha))) {
            ++result.verified;
        } else {
            ++result.mismatched;
        }
    }
    return result;
}

ContainerHeader parseContainerHeader(std::string_view data) {
    Cursor c(data);
    ContainerHeader header;
    const std::uint32_t first = c.u32();
    if (!c.ok()) return fail<ContainerHeader>(ParseStatus::Malformed, "Empty container header.");
    if (first == kContainerHeaderSignature) {
        header.hasSignature = true;
        header.version = c.u32();
        header.containerId = c.u64();
        const std::int32_t count = c.i32();
        if (!c.ok() || count < 0 || static_cast<std::uint64_t>(count) * 8 > c.remaining()) {
            return fail<ContainerHeader>(ParseStatus::Malformed, "Unreadable package list.");
        }
        header.packageIds.resize(static_cast<std::size_t>(count));
        for (auto& id : header.packageIds) id = c.u64();
        header.status = ParseStatus::Parsed;
        return header;
    }
    // UE 5.0 had no signature: only the container id is read.
    Cursor legacy(data);
    header.containerId = legacy.u64();
    header.status = legacy.ok() ? ParseStatus::Parsed : ParseStatus::Malformed;
    header.detail = "Container header without signature (UE 5.0 layout): only the container id was read.";
    return header;
}

// ------------------------------------------------------------------------------- packages

namespace {

// FNameBatch: count, string bytes, hash algorithm, hashes, headers, strings.
bool readNameBatch(Cursor& c, std::vector<std::string>& names, std::size_t maxNames) {
    const std::uint32_t count = c.u32();
    if (!c.ok()) return false;
    if (count == 0) return true;
    if (count > maxNames) return c.fail();
    const std::uint32_t stringBytes = c.u32();
    (void)c.u64();  // hash algorithm id
    if (!c.skip(static_cast<std::uint64_t>(count) * 8)) return false;
    const std::string_view headers = c.bytes(static_cast<std::size_t>(count) * 2);
    const std::string_view strings = c.bytes(stringBytes);
    if (!c.ok()) return false;
    std::size_t position = 0;
    names.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto h0 = static_cast<unsigned char>(headers[2 * i]);
        const auto h1 = static_cast<unsigned char>(headers[2 * i + 1]);
        const bool wide = (h0 & 0x80) != 0;
        const std::size_t length = (static_cast<std::size_t>(h0 & 0x7F) << 8) | h1;
        if (wide) {
            position += position & 1;  // UTF-16 strings start on an even offset
            if (length * 2 > strings.size() || position > strings.size() - length * 2) return c.fail();
            names.push_back(Cursor::utf16ToUtf8(strings.substr(position, length * 2)));
            position += length * 2;
        } else {
            if (length > strings.size() || position > strings.size() - length) return c.fail();
            names.push_back(Cursor::latin1ToUtf8(strings.substr(position, length)));
            position += length;
        }
    }
    return true;
}

std::string mappedName(const std::vector<std::string>& names, std::uint32_t index, std::uint32_t number) {
    const std::uint32_t position = index & 0x3FFFFFFFu;
    if (position >= names.size()) return {};
    return number == 0 ? names[position] : strings::concat(names[position], "_", number - 1);
}

}  // namespace

ZenPackage parseZenPackage(std::string_view data) {
    Cursor c(data);
    ZenPackage package;
    const std::uint32_t hasVersioning = c.u32();
    package.headerSize = c.u32();
    const std::uint32_t nameIndex = c.u32();
    const std::uint32_t nameNumber = c.u32();
    package.packageFlags = c.u32();
    (void)c.u32();  // cooked header size
    const std::int32_t importedHashes = c.i32();
    const std::int32_t importMap = c.i32();
    const std::int32_t exportMap = c.i32();
    const std::int32_t exportBundles = c.i32();
    const std::int32_t next1 = c.i32();
    const std::int32_t next2 = c.i32();
    const std::int32_t next3 = c.i32();
    if (!c.ok() || hasVersioning > 1 || package.headerSize > data.size() || package.headerSize < 44) {
        return fail<ZenPackage>(ParseStatus::Malformed, "Not a readable Zen package header.");
    }
    package.hasVersioningInfo = hasVersioning == 1;
    const auto size = static_cast<std::int64_t>(package.headerSize);
    auto ordered = [&](std::initializer_list<std::int64_t> values) {
        std::int64_t previous = 0;
        for (std::int64_t value : values) {
            if (value < previous || value > size) return false;
            previous = value;
        }
        return true;
    };
    std::size_t summarySize = 0;
    std::int32_t importedNamesOffset = -1;
    if (package.headerSize >= 52 && ordered({52, importedHashes, importMap, exportMap, exportBundles, next1, next2, next3})) {
        summarySize = 52;
        importedNamesOffset = next3;
        package.layout = "UE 5.3+";
    } else if (ordered({44, importedHashes, importMap, exportMap, exportBundles, next1})) {
        summarySize = 44;
        package.layout = "UE 5.0-5.2";
    } else {
        return fail<ZenPackage>(ParseStatus::Unsupported, "Unknown Zen package summary layout.");
    }
    Cursor body(data.substr(0, package.headerSize), summarySize);
    if (package.hasVersioningInfo) {
        (void)body.u32();  // zen version
        (void)body.i32();  // UE4 file version
        (void)body.i32();  // UE5 file version
        (void)body.i32();  // licensee version
        const std::int32_t customVersions = body.i32();
        if (!body.ok() || customVersions < 0 || customVersions > 4096 || !body.skip(static_cast<std::uint64_t>(customVersions) * 20)) {
            return fail<ZenPackage>(ParseStatus::Malformed, "Unreadable versioning information.");
        }
    }
    std::vector<std::string> names;
    if (!readNameBatch(body, names, 1u << 20)) return fail<ZenPackage>(ParseStatus::Malformed, "Unreadable name map.");
    package.nameCount = names.size();
    package.name = mappedName(names, nameIndex, nameNumber);
    if (package.name.empty()) return fail<ZenPackage>(ParseStatus::Malformed, "The package name is not in the name map.");
    if (importedNamesOffset > 0 && importedNamesOffset < static_cast<std::int32_t>(package.headerSize)) {
        Cursor imports(data.substr(0, package.headerSize), static_cast<std::size_t>(importedNamesOffset));
        std::vector<std::string> imported;
        if (readNameBatch(imports, imported, 65536)) {
            package.importedPackages = std::move(imported);
            package.importsParsed = true;
        }
    } else if (importedNamesOffset == static_cast<std::int32_t>(package.headerSize)) {
        package.importsParsed = true;  // no imported packages
    }
    package.status = ParseStatus::Parsed;
    return package;
}

LegacyPackageSummary parseLegacyPackageSummary(std::string_view head) {
    Cursor c(head);
    LegacyPackageSummary summary;
    const std::uint32_t tag = c.u32();
    if (!c.ok()) return fail<LegacyPackageSummary>(ParseStatus::Malformed, "Too small.");
    if (tag == 0xC1832A9Eu) return fail<LegacyPackageSummary>(ParseStatus::Unsupported, "Big-endian package.");
    if (tag != kPackageFileTag) return fail<LegacyPackageSummary>(ParseStatus::Malformed, "No Unreal package tag.");
    summary.legacyFileVersion = c.i32();
    if (summary.legacyFileVersion >= 0 || summary.legacyFileVersion < -9) {
        return fail<LegacyPackageSummary>(ParseStatus::Unsupported,
                                          strings::concat("Unknown package file version ", summary.legacyFileVersion, "."));
    }
    if (summary.legacyFileVersion != -4) (void)c.i32();  // UE3 version
    summary.fileVersionUE4 = c.i32();
    if (summary.legacyFileVersion <= -8) summary.fileVersionUE5 = c.i32();
    summary.licenseeVersion = c.i32();
    if (!c.ok()) return fail<LegacyPackageSummary>(ParseStatus::Malformed, "Truncated package summary.");
    summary.status = ParseStatus::Parsed;
    return summary;
}

}  // namespace akeno::unreal
