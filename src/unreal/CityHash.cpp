// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/unreal/CityHash.hpp"

#include <string>
#include <utility>

namespace akeno::unreal {

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;

constexpr u64 k0 = 0xc3a5c85c97cb3127ULL;
constexpr u64 k1 = 0xb492b66fbe98f273ULL;
constexpr u64 k2 = 0x9ae16a3b2f90404fULL;

u64 fetch64(const char* p) {
    u64 value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(p[i]);
    return value;
}

u32 fetch32(const char* p) {
    u32 value = 0;
    for (int i = 3; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(p[i]);
    return value;
}

u64 byteSwap(u64 value) {
    u64 out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | (value & 0xff);
        value >>= 8;
    }
    return out;
}

u64 rotate(u64 value, int shift) { return shift == 0 ? value : ((value >> shift) | (value << (64 - shift))); }
u64 shiftMix(u64 value) { return value ^ (value >> 47); }

u64 hashLen16(u64 u, u64 v, u64 mul) {
    u64 a = (u ^ v) * mul;
    a ^= (a >> 47);
    u64 b = (v ^ a) * mul;
    b ^= (b >> 47);
    b *= mul;
    return b;
}

u64 hashLen16(u64 u, u64 v) { return hashLen16(u, v, 0x9ddfea08eb382d69ULL); }

u64 hashLen0to16(const char* s, std::size_t len) {
    if (len >= 8) {
        const u64 mul = k2 + len * 2;
        const u64 a = fetch64(s) + k2;
        const u64 b = fetch64(s + len - 8);
        const u64 c = rotate(b, 37) * mul + a;
        const u64 d = (rotate(a, 25) + b) * mul;
        return hashLen16(c, d, mul);
    }
    if (len >= 4) {
        const u64 mul = k2 + len * 2;
        const u64 a = fetch32(s);
        return hashLen16(len + (a << 3), fetch32(s + len - 4), mul);
    }
    if (len > 0) {
        const auto a = static_cast<unsigned char>(s[0]);
        const auto b = static_cast<unsigned char>(s[len >> 1]);
        const auto c = static_cast<unsigned char>(s[len - 1]);
        const u32 y = static_cast<u32>(a) + (static_cast<u32>(b) << 8);
        const u32 z = static_cast<u32>(len) + (static_cast<u32>(c) << 2);
        return shiftMix(y * k2 ^ z * k0) * k2;
    }
    return k2;
}

u64 hashLen17to32(const char* s, std::size_t len) {
    const u64 mul = k2 + len * 2;
    const u64 a = fetch64(s) * k1;
    const u64 b = fetch64(s + 8);
    const u64 c = fetch64(s + len - 8) * mul;
    const u64 d = fetch64(s + len - 16) * k2;
    return hashLen16(rotate(a + b, 43) + rotate(c, 30) + d, a + rotate(b + k2, 18) + c, mul);
}

std::pair<u64, u64> weakHashLen32WithSeeds(u64 w, u64 x, u64 y, u64 z, u64 a, u64 b) {
    a += w;
    b = rotate(b + a + z, 21);
    const u64 c = a;
    a += x;
    a += y;
    b += rotate(a, 44);
    return {a + z, b + c};
}

std::pair<u64, u64> weakHashLen32WithSeeds(const char* s, u64 a, u64 b) {
    return weakHashLen32WithSeeds(fetch64(s), fetch64(s + 8), fetch64(s + 16), fetch64(s + 24), a, b);
}

u64 hashLen33to64(const char* s, std::size_t len) {
    const u64 mul = k2 + len * 2;
    u64 a = fetch64(s) * k2;
    u64 b = fetch64(s + 8);
    const u64 c = fetch64(s + len - 24);
    const u64 d = fetch64(s + len - 32);
    const u64 e = fetch64(s + 16) * k2;
    const u64 f = fetch64(s + 24) * 9;
    const u64 g = fetch64(s + len - 8);
    const u64 h = fetch64(s + len - 16) * mul;
    const u64 u = rotate(a + g, 43) + (rotate(b, 30) + c) * 9;
    const u64 v = ((a + g) ^ d) + f + 1;
    const u64 w = byteSwap((u + v) * mul) + h;
    const u64 x = rotate(e + f, 42) + c;
    const u64 y = (byteSwap((v + w) * mul) + g) * mul;
    const u64 z = e + f + c;
    a = byteSwap((x + z) * mul + y) + b;
    b = shiftMix((z + a) * mul + d + h) * mul;
    return b + x;
}

}  // namespace

std::uint64_t cityHash64(const char* s, std::size_t len) {
    if (len <= 32) {
        return len <= 16 ? hashLen0to16(s, len) : hashLen17to32(s, len);
    }
    if (len <= 64) return hashLen33to64(s, len);

    u64 x = fetch64(s + len - 40);
    u64 y = fetch64(s + len - 16) + fetch64(s + len - 56);
    u64 z = hashLen16(fetch64(s + len - 48) + len, fetch64(s + len - 24));
    auto v = weakHashLen32WithSeeds(s + len - 64, len, z);
    auto w = weakHashLen32WithSeeds(s + len - 32, y + k1, x);
    x = x * k1 + fetch64(s);
    len = (len - 1) & ~static_cast<std::size_t>(63);
    do {
        x = rotate(x + y + v.first + fetch64(s + 8), 37) * k1;
        y = rotate(y + v.second + fetch64(s + 48), 42) * k1;
        x ^= w.second;
        y += v.first + fetch64(s + 40);
        z = rotate(z + w.first, 33) * k1;
        v = weakHashLen32WithSeeds(s, v.second * k1, x + w.first);
        w = weakHashLen32WithSeeds(s + 32, z + w.second, y + fetch64(s + 16));
        std::swap(z, x);
        s += 64;
        len -= 64;
    } while (len != 0);
    return hashLen16(hashLen16(v.first, w.first) + shiftMix(y) * k1 + z, hashLen16(v.second, w.second) + x);
}

std::uint64_t packageIdFromName(std::string_view packageName) {
    std::string utf16;
    utf16.reserve(packageName.size() * 2);
    for (char c : packageName) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x80) return 0;
        utf16.push_back(static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + 32 : byte));
        utf16.push_back('\0');
    }
    return cityHash64(utf16);
}

}  // namespace akeno::unreal
