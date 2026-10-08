// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/Digests.hpp"

#include <algorithm>
#include <cstring>

#include <openssl/evp.h>

namespace akeno::security {

Sha1Digest sha1(std::string_view data) {
    Sha1Digest digest{};
    unsigned int length = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx != nullptr && EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, data.data(), data.size()) == 1) {
        unsigned char out[EVP_MAX_MD_SIZE];
        if (EVP_DigestFinal_ex(ctx, out, &length) == 1 && length == digest.size()) {
            std::memcpy(digest.data(), out, digest.size());
        }
    }
    EVP_MD_CTX_free(ctx);
    return digest;
}

namespace {

constexpr std::array<std::uint32_t, 8> kIv{0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                           0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
constexpr std::array<std::uint8_t, 16> kPermutation{2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};
constexpr std::uint32_t kChunkStart = 1, kChunkEnd = 2, kParent = 4, kRoot = 8;
constexpr std::size_t kBlockLength = 64, kChunkLength = 1024;

using Words16 = std::array<std::uint32_t, 16>;
using Cv = std::array<std::uint32_t, 8>;

constexpr std::uint32_t rotr(std::uint32_t value, int shift) { return (value >> shift) | (value << (32 - shift)); }

void g(Words16& s, int a, int b, int c, int d, std::uint32_t x, std::uint32_t y) {
    s[a] = s[a] + s[b] + x;
    s[d] = rotr(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = rotr(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + y;
    s[d] = rotr(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = rotr(s[b] ^ s[c], 7);
}

Words16 compress(const Cv& cv, const Words16& blockWords, std::uint64_t counter, std::uint32_t blockLength,
                 std::uint32_t flags) {
    Words16 s{cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
              kIv[0], kIv[1], kIv[2], kIv[3], static_cast<std::uint32_t>(counter),
              static_cast<std::uint32_t>(counter >> 32), blockLength, flags};
    Words16 m = blockWords;
    for (int round = 0; round < 7; ++round) {
        g(s, 0, 4, 8, 12, m[0], m[1]);
        g(s, 1, 5, 9, 13, m[2], m[3]);
        g(s, 2, 6, 10, 14, m[4], m[5]);
        g(s, 3, 7, 11, 15, m[6], m[7]);
        g(s, 0, 5, 10, 15, m[8], m[9]);
        g(s, 1, 6, 11, 12, m[10], m[11]);
        g(s, 2, 7, 8, 13, m[12], m[13]);
        g(s, 3, 4, 9, 14, m[14], m[15]);
        if (round < 6) {
            Words16 permuted{};
            for (std::size_t i = 0; i < 16; ++i) permuted[i] = m[kPermutation[i]];
            m = permuted;
        }
    }
    for (std::size_t i = 0; i < 8; ++i) {
        s[i] ^= s[i + 8];
        s[i + 8] ^= cv[i];
    }
    return s;
}

Words16 wordsOf(const std::uint8_t* block) {
    Words16 words{};
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = static_cast<std::uint32_t>(block[4 * i]) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[4 * i + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[4 * i + 3]) << 24);
    }
    return words;
}

Cv firstEight(const Words16& words) {
    Cv cv{};
    std::copy_n(words.begin(), 8, cv.begin());
    return cv;
}

struct Output {
    Cv inputCv{};
    Words16 blockWords{};
    std::uint64_t counter = 0;
    std::uint32_t blockLength = 0;
    std::uint32_t flags = 0;

    Cv chainingValue() const { return firstEight(compress(inputCv, blockWords, counter, blockLength, flags)); }
    void rootBytes(std::uint8_t* out, std::size_t size) const {
        std::uint64_t outputCounter = 0;
        std::size_t written = 0;
        while (written < size) {
            const Words16 words = compress(inputCv, blockWords, outputCounter++, blockLength, flags | kRoot);
            for (std::size_t i = 0; i < 16 && written < size; ++i) {
                for (int byte = 0; byte < 4 && written < size; ++byte) {
                    out[written++] = static_cast<std::uint8_t>(words[i] >> (8 * byte));
                }
            }
        }
    }
};

Output parentOutput(const Cv& left, const Cv& right) {
    Output output;
    output.inputCv = kIv;
    std::copy(left.begin(), left.end(), output.blockWords.begin());
    std::copy(right.begin(), right.end(), output.blockWords.begin() + 8);
    output.blockLength = kBlockLength;
    output.flags = kParent;
    return output;
}

}  // namespace

Blake3::Blake3() { resetChunk(0); }

void Blake3::resetChunk(std::uint64_t counter) {
    chunk_ = ChunkState{};
    chunk_.cv = kIv;
    chunk_.counter = counter;
}

void Blake3::addChunkValue(std::array<std::uint32_t, 8> cv, std::uint64_t totalChunks) {
    // Merge completed subtrees: one merge per trailing zero bit of the chunk count.
    while ((totalChunks & 1) == 0 && stackSize_ > 0) {
        cv = parentOutput(stack_[--stackSize_], cv).chainingValue();
        totalChunks >>= 1;
    }
    stack_[stackSize_++] = cv;
}

void Blake3::update(const void* data, std::size_t size) {
    const auto* input = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        if (chunk_.length() == kChunkLength) {
            Output output;
            output.inputCv = chunk_.cv;
            output.blockWords = wordsOf(chunk_.block.data());
            output.counter = chunk_.counter;
            output.blockLength = chunk_.blockLength;
            output.flags = (chunk_.blocksCompressed == 0 ? kChunkStart : 0u) | kChunkEnd;
            const std::uint64_t total = chunk_.counter + 1;
            addChunkValue(output.chainingValue(), total);
            resetChunk(total);
        }
        // Compress a full block only when more input follows: the last block needs CHUNK_END.
        if (chunk_.blockLength == kBlockLength) {
            const std::uint32_t flags = chunk_.blocksCompressed == 0 ? kChunkStart : 0u;
            chunk_.cv = firstEight(compress(chunk_.cv, wordsOf(chunk_.block.data()), chunk_.counter,
                                            static_cast<std::uint32_t>(kBlockLength), flags));
            ++chunk_.blocksCompressed;
            chunk_.block.fill(0);
            chunk_.blockLength = 0;
        }
        const std::size_t take = std::min(kBlockLength - chunk_.blockLength, size);
        std::memcpy(chunk_.block.data() + chunk_.blockLength, input, take);
        chunk_.blockLength = static_cast<std::uint8_t>(chunk_.blockLength + take);
        input += take;
        size -= take;
    }
}

void Blake3::finish(std::uint8_t* out, std::size_t size) {
    Output output;
    output.inputCv = chunk_.cv;
    output.blockWords = wordsOf(chunk_.block.data());
    output.counter = chunk_.counter;
    output.blockLength = chunk_.blockLength;
    output.flags = (chunk_.blocksCompressed == 0 ? kChunkStart : 0u) | kChunkEnd;
    std::size_t remaining = stackSize_;
    while (remaining > 0) {
        output = parentOutput(stack_[--remaining], output.chainingValue());
    }
    output.rootBytes(out, size);
}

std::array<std::uint8_t, 32> blake3(std::string_view data) {
    Blake3 hasher;
    hasher.update(data);
    std::array<std::uint8_t, 32> digest{};
    hasher.finish(digest.data(), digest.size());
    return digest;
}

std::array<std::uint8_t, 20> blake3_160(std::string_view data) {
    Blake3 hasher;
    hasher.update(data);
    std::array<std::uint8_t, 20> digest{};
    hasher.finish(digest.data(), digest.size());
    return digest;
}

std::string toHex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        hex.push_back(kHex[data[i] >> 4]);
        hex.push_back(kHex[data[i] & 0x0F]);
    }
    return hex;
}

}  // namespace akeno::security
