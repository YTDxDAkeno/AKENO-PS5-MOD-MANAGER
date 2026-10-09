// SPDX-License-Identifier: GPL-3.0-or-later
// Builds small, structurally valid Unreal Engine containers for tests: .pak (IoStore companion
// stub, version 11, and legacy version 8 with stored files), IoStore .utoc/.ucas (version 8 with
// BLAKE3-160 chunk hashes, or older versions), container headers and Zen package headers. The
// payloads are synthetic; no game data is involved.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "akeno/security/Digests.hpp"
#include "akeno/unreal/CityHash.hpp"

namespace akeno::test {

class Bytes {
public:
    Bytes& u8(std::uint8_t v) { data_.push_back(static_cast<char>(v)); return *this; }
    Bytes& le(std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) data_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    Bytes& be(std::uint64_t v, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) data_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    Bytes& u16(std::uint16_t v) { return le(v, 2); }
    Bytes& u32(std::uint32_t v) { return le(v, 4); }
    Bytes& i32(std::int32_t v) { return le(static_cast<std::uint32_t>(v), 4); }
    Bytes& u64(std::uint64_t v) { return le(v, 8); }
    Bytes& raw(std::string_view text) { data_.append(text); return *this; }
    Bytes& zeros(std::size_t count) { data_.append(count, '\0'); return *this; }
    Bytes& fstring(std::string_view text) {
        i32(static_cast<std::int32_t>(text.size() + 1));
        data_.append(text);
        data_.push_back('\0');
        return *this;
    }
    std::size_t size() const { return data_.size(); }
    const std::string& str() const { return data_; }

private:
    std::string data_;
};

inline std::string sha1Bytes(std::string_view data) {
    const auto digest = security::sha1(data);
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

inline std::string pakFooter(int version, std::uint64_t indexOffset, const std::string& index, int methods = 5,
                             std::vector<std::string> methodNames = {}) {
    Bytes footer;
    if (version >= 7) footer.zeros(16);
    footer.u8(0).u32(0x5A6F12E1).i32(version).u64(indexOffset).u64(index.size()).raw(sha1Bytes(index));
    if (version == 9) footer.u8(0);
    if (version >= 8) {
        for (int i = 0; i < methods; ++i) {
            std::string name = i < static_cast<int>(methodNames.size()) ? methodNames[static_cast<std::size_t>(i)] : "";
            name.resize(32, '\0');
            footer.raw(name);
        }
    }
    return footer.str();
}

// The .pak the cooker writes next to an IoStore container: a mount point and no files.
inline std::string pakStub(const std::string& mount = "../../../") {
    const std::string pathHashIndex(8, '\0');
    const std::string directoryIndex(4, '\0');
    Bytes head;
    head.fstring(mount).i32(0).u64(0);
    const std::uint64_t size = head.size() + 4 + 36 + 4 + 36 + 4 + 4;
    Bytes index;
    index.raw(head.str()).u32(1).u64(size).u64(pathHashIndex.size()).raw(sha1Bytes(pathHashIndex));
    index.u32(1).u64(size + pathHashIndex.size()).u64(directoryIndex.size()).raw(sha1Bytes(directoryIndex));
    index.i32(0).i32(0);
    return index.str() + pathHashIndex + directoryIndex + pakFooter(11, 0, index.str());
}

// A legacy (version 8) .pak with stored files: {path relative to the mount point, data}.
inline std::string legacyPak(const std::vector<std::pair<std::string, std::string>>& files,
                             const std::string& mount = "../../../") {
    Bytes blob, entries;
    for (const auto& [path, data] : files) {
        Bytes entry;
        entry.u64(blob.size()).u64(data.size()).u64(data.size()).u32(0).raw(sha1Bytes(data)).u8(0).u32(0);
        blob.raw(entry.str()).raw(data);
        entries.fstring(path).raw(entry.str());
    }
    Bytes index;
    index.fstring(mount).i32(static_cast<std::int32_t>(files.size())).raw(entries.str());
    return blob.str() + index.str() + pakFooter(8, blob.size(), index.str());
}

struct ChunkSpec {
    std::uint64_t id = 0;
    std::uint8_t type = 1;  // ExportBundleData
    std::string data;
};

struct IoStoreFiles {
    std::string utoc;
    std::string ucas;
};

// An uncompressed, unencrypted, single-partition IoStore container with a directory index that
// lists `fileName` (chunk 0) under `mount`. `version` < 8 stores 32-byte hashes (BLAKE3-256).
inline IoStoreFiles ioStore(std::uint64_t containerId, const std::vector<ChunkSpec>& chunks,
                            const std::string& fileName = "Asset.uasset", int version = 8,
                            const std::string& mount = "../../../", std::uint32_t blockSize = 65536) {
    IoStoreFiles out;
    struct Block { std::uint64_t offset; std::uint32_t size; };
    std::vector<Block> blocks;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> offsets;
    std::uint64_t virtualOffset = 0;
    for (const auto& chunk : chunks) {
        offsets.emplace_back(virtualOffset, chunk.data.size());
        const std::size_t count = std::max<std::size_t>(1, (chunk.data.size() + blockSize - 1) / blockSize);
        for (std::size_t i = 0; i < count; ++i) {
            const std::string piece = chunk.data.substr(std::min(chunk.data.size(), i * blockSize), blockSize);
            blocks.push_back({out.ucas.size(), static_cast<std::uint32_t>(piece.size())});
            out.ucas += piece;
        }
        virtualOffset += count * blockSize;
    }
    Bytes directory;
    directory.fstring(mount).i32(1).u32(0xFFFFFFFFu).u32(0xFFFFFFFFu).u32(0xFFFFFFFFu).u32(0);
    directory.i32(1).u32(0).u32(0xFFFFFFFFu).u32(0).i32(1).fstring(fileName);
    Bytes toc;
    toc.raw("-==--==--==--==-").u8(static_cast<std::uint8_t>(version)).u8(0).u16(0).u32(144);
    toc.u32(static_cast<std::uint32_t>(chunks.size())).u32(static_cast<std::uint32_t>(blocks.size())).u32(12).u32(0).u32(32);
    toc.u32(blockSize).u32(static_cast<std::uint32_t>(directory.size())).u32(1).u64(containerId).zeros(16);
    toc.u8(0x08).u8(0).u16(0).u32(0).u64(~0ull).u32(0).u32(0).zeros(40);
    for (const auto& chunk : chunks) toc.u64(chunk.id).be(0, 2).u8(0).u8(chunk.type);
    for (const auto& [offset, length] : offsets) toc.be(offset, 5).be(length, 5);
    for (const auto& block : blocks) toc.le(block.offset, 5).le(block.size, 3).le(block.size, 3).u8(0);
    toc.raw(directory.str());
    for (const auto& chunk : chunks) {
        if (version >= 8) {
            const auto hash = security::blake3_160(chunk.data);
            toc.raw(std::string(reinterpret_cast<const char*>(hash.data()), hash.size())).u8(0).zeros(3);
        } else {
            const auto hash = security::blake3(chunk.data);
            toc.raw(std::string(reinterpret_cast<const char*>(hash.data()), hash.size())).u8(0);
        }
    }
    out.utoc = toc.str();
    return out;
}

inline std::string containerHeader(std::uint64_t containerId, const std::vector<std::uint64_t>& packages) {
    Bytes header;
    header.u32(0x496f436e).u32(4).u64(containerId).i32(static_cast<std::int32_t>(packages.size()));
    for (auto id : packages) header.u64(id);
    header.zeros(16);
    return header.str();
}

inline std::string nameBatch(const std::vector<std::string>& names) {
    Bytes batch;
    batch.u32(static_cast<std::uint32_t>(names.size()));
    if (names.empty()) return batch.str();
    std::size_t stringBytes = 0;
    for (const auto& name : names) stringBytes += name.size();
    batch.u32(static_cast<std::uint32_t>(stringBytes)).u64(0xC1640000ull);
    for (const auto& name : names) batch.u64(unreal::cityHash64(name));
    for (const auto& name : names) batch.u8(static_cast<std::uint8_t>(name.size() >> 8)).u8(static_cast<std::uint8_t>(name.size() & 0xFF));
    for (const auto& name : names) batch.raw(name);
    return batch.str();
}

// A cooked, unversioned UE 5.3+ Zen package header named `name` with `imports`, followed by
// `payload` (stand-in for export data).
inline std::string zenPackage(const std::string& name, const std::vector<std::string>& imports,
                              const std::string& payload = std::string(64, 'x')) {
    std::vector<std::string> names{"None", "Default__Object", name};
    const std::string nameMap = nameBatch(names);
    Bytes importsBatch;
    importsBatch.raw(nameBatch(imports));
    for (std::size_t i = 0; i < imports.size(); ++i) importsBatch.i32(0);
    const std::uint32_t summary = 52;
    const std::uint32_t afterNames = summary + static_cast<std::uint32_t>(nameMap.size());
    const std::uint32_t sections = afterNames + 16;  // four empty-ish sections of 4 bytes each
    const std::uint32_t headerSize = sections + static_cast<std::uint32_t>(importsBatch.size());
    Bytes package;
    package.u32(0).u32(headerSize).u32(2).u32(0).u32(0x80002200u).u32(headerSize);
    package.i32(static_cast<std::int32_t>(afterNames)).i32(static_cast<std::int32_t>(afterNames + 4));
    package.i32(static_cast<std::int32_t>(afterNames + 8)).i32(static_cast<std::int32_t>(afterNames + 12));
    package.i32(static_cast<std::int32_t>(sections)).i32(static_cast<std::int32_t>(sections));
    package.i32(static_cast<std::int32_t>(sections));
    package.raw(nameMap).zeros(16).raw(importsBatch.str()).raw(payload);
    return package.str();
}

// A complete package set {pak, utoc, ucas} for one package.
struct PackageSetFiles {
    std::string pak, utoc, ucas;
    std::uint64_t containerId = 0, packageId = 0;
};

// `bulkData`: also a BulkData chunk for the package (as textures, meshes and audio have).
inline PackageSetFiles packageSet(const std::string& packageName, const std::vector<std::string>& imports,
                                  std::uint64_t containerId = 0xe4a7420854dad984ull, bool bulkData = false) {
    PackageSetFiles set;
    set.containerId = containerId;
    set.packageId = unreal::packageIdFromName(packageName);
    const std::string assetFile = packageName.substr(packageName.rfind('/') + 1) + ".uasset";
    std::vector<ChunkSpec> chunks{{set.packageId, 1, zenPackage(packageName, imports)}};
    if (bulkData) chunks.push_back({set.packageId, 2, std::string(4096, 't')});
    chunks.push_back({containerId, 6, containerHeader(containerId, {set.packageId})});
    auto files = ioStore(containerId, chunks, assetFile);
    set.pak = pakStub();
    set.utoc = std::move(files.utoc);
    set.ucas = std::move(files.ucas);
    return set;
}

}  // namespace akeno::test
