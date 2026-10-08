// SPDX-License-Identifier: GPL-3.0-or-later
// Digests that container formats embed: SHA-1 (Unreal .pak index hashes, OpenSSL EVP) and
// BLAKE3 (Unreal IoStore chunk hashes, FIoHash = the first 20 bytes of BLAKE3-256). They are
// used only to check that a container agrees with itself, never as a security boundary.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace akeno::security {

using Sha1Digest = std::array<std::uint8_t, 20>;
Sha1Digest sha1(std::string_view data);

// Portable, single-threaded BLAKE3 (unkeyed hash mode) written from the specification.
class Blake3 {
public:
    Blake3();
    void update(const void* data, std::size_t size);
    void update(std::string_view data) { update(data.data(), data.size()); }
    // Writes `size` bytes of output (32 is the standard digest). May be called once.
    void finish(std::uint8_t* out, std::size_t size);

private:
    struct ChunkState {
        std::array<std::uint32_t, 8> cv{};
        std::uint64_t counter = 0;
        std::array<std::uint8_t, 64> block{};
        std::uint8_t blockLength = 0;
        std::uint8_t blocksCompressed = 0;
        std::size_t length() const { return 64u * blocksCompressed + blockLength; }
    };
    void resetChunk(std::uint64_t counter);
    void addChunkValue(std::array<std::uint32_t, 8> cv, std::uint64_t totalChunks);

    ChunkState chunk_;
    std::array<std::array<std::uint32_t, 8>, 54> stack_{};
    std::size_t stackSize_ = 0;
};

std::array<std::uint8_t, 32> blake3(std::string_view data);
// FIoHash: BLAKE3-256 truncated to 160 bits.
std::array<std::uint8_t, 20> blake3_160(std::string_view data);

std::string toHex(const std::uint8_t* data, std::size_t size);
template <std::size_t N>
std::string toHex(const std::array<std::uint8_t, N>& digest) {
    return toHex(digest.data(), N);
}

}  // namespace akeno::security
