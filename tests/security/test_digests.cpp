// SPDX-License-Identifier: GPL-3.0-or-later
// Reference values were produced with the upstream BLAKE3 and Google CityHash 1.1
// implementations (Python bindings) on the input pattern byte[i] = i % 251.
#include "Doctest.hpp"

#include <string>
#include <utility>
#include <vector>

#include "akeno/security/Digests.hpp"
#include "akeno/unreal/CityHash.hpp"

using namespace akeno;

namespace {

std::string pattern(std::size_t length) {
    std::string data(length, '\0');
    for (std::size_t i = 0; i < length; ++i) data[i] = static_cast<char>(i % 251);
    return data;
}

}  // namespace

TEST_CASE("SHA-1 matches the FIPS 180 examples") {
    CHECK(security::toHex(security::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(security::toHex(security::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("BLAKE3 matches the reference implementation across chunk and tree boundaries") {
    const std::vector<std::pair<std::size_t, const char*>> vectors{
        {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {63, "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
        {64, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {65, "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
        {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
        {2049, "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
        {3072, "b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2"},
        {3073, "7124b49501012f81cc7f11ca069ec9226cecb8a2c850cfe644e327d22d3e1cd3"},
        {4096, "015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969"},
        {4097, "9b4052b38f1c5fc8b1f9ff7ac7b27cd242487b3d890d15c96a1c25b8aa0fb995"},
        {5120, "9cadc15fed8b5d854562b26a9536d9707cadeda9b143978f319ab34230535833"},
        {8193, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b"},
        {31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
        {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
    };
    for (const auto& [length, expected] : vectors) {
        CAPTURE(length);
        const std::string data = pattern(length);
        CHECK(security::toHex(security::blake3(data)) == expected);
        // Streaming in odd pieces gives the same digest.
        security::Blake3 hasher;
        for (std::size_t offset = 0; offset < data.size(); offset += 97) {
            hasher.update(data.data() + offset, std::min<std::size_t>(97, data.size() - offset));
        }
        std::array<std::uint8_t, 32> streamed{};
        hasher.finish(streamed.data(), streamed.size());
        CHECK(security::toHex(streamed) == expected);
        CHECK(security::toHex(security::blake3_160(data)) == std::string(expected).substr(0, 40));
    }
}

TEST_CASE("CityHash64 matches CityHash 1.1 for every length class") {
    const std::vector<std::pair<std::size_t, std::uint64_t>> vectors{
        {0, 0x9ae16a3b2f90404fULL},   {1, 0xbe6056edf5e94b54ULL},   {2, 0xc2a04665ed038d75ULL},
        {3, 0x94a13d22e9eba49aULL},   {4, 0x82bffd898958e540ULL},   {7, 0xa2e0bff20db0a6a1ULL},
        {8, 0xad5a13e1e8e93b98ULL},   {15, 0x862a51555943bd9dULL},  {16, 0x0efd25a0a34156d4ULL},
        {17, 0xbbb6a6f8f20d1f1cULL},  {31, 0xfbd950af27ef6941ULL},  {32, 0x1a9d8199972cdf49ULL},
        {33, 0x46e1378cbc22dabaULL},  {63, 0xaf30927a77ada6efULL},  {64, 0xe99ab80f5ec7dca5ULL},
        {65, 0xac589c990483dd2eULL},  {127, 0xefd614390a7b1d95ULL}, {128, 0x10b153630af1f395ULL},
        {129, 0x46be8f236f918770ULL}, {200, 0xf4d24e8c7493c3d8ULL}, {1000, 0xc7b11baab7f2974aULL},
    };
    for (const auto& [length, expected] : vectors) {
        CAPTURE(length);
        CHECK(unreal::cityHash64(pattern(length)) == expected);
    }
}

TEST_CASE("Unreal package ids derive from the lower-case UTF-16 package name") {
    // Observed in the IoStore container of the Better Carry Weight x10 mod (chunk id of its
    // only ExportBundleData chunk).
    CHECK(unreal::packageIdFromName("/Game/_Dawnwalker/Player/BP_PlayerCharacter") == 0x7285fde174bd91f8ULL);
    CHECK(unreal::packageIdFromName("/game/_dawnwalker/player/bp_playercharacter") == 0x7285fde174bd91f8ULL);
    CHECK(unreal::packageIdFromName("/Game/\xC3\xA9") == 0);
}
