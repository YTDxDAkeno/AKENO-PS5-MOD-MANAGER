// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "UnrealTestSupport.hpp"
#include "akeno/unreal/UnrealAnalyzer.hpp"
#include "akeno/unreal/UnrealFormats.hpp"

using namespace akeno;
using namespace akeno::unreal;

namespace {

MemorySource source(std::string data) { return MemorySource(std::move(data)); }

std::string corrupt(std::string data, std::size_t at) {
    data[at] = static_cast<char>(data[at] ^ 0x5A);
    return data;
}

}  // namespace

TEST_CASE("an IoStore companion .pak (version 11) parses with verified index hashes") {
    const auto pak = parsePak(source(test::pakStub()));
    REQUIRE(pak.status == ParseStatus::Parsed);
    CHECK(pak.version == 11);
    CHECK(pak.mountPoint == "../../../");
    CHECK(pak.entryCount == 0);
    CHECK(pak.indexHashVerified);
    CHECK(pak.secondaryIndexesVerified);
    CHECK_FALSE(pak.encryptedIndex);
    CHECK(pak.encryptionKeyGuid.empty());
}

TEST_CASE("a legacy .pak lists its files") {
    const auto pak = parsePak(source(test::legacyPak({{"Game/Content/Characters/Hero.uasset", "data"},
                                                      {"Game/Content/Characters/Hero.uexp", "more"}})));
    REQUIRE(pak.status == ParseStatus::Parsed);
    CHECK(pak.version == 8);
    CHECK(pak.entryCount == 2);
    CHECK(pak.filesComplete);
    CHECK(pak.files == std::vector<std::string>{"../../../Game/Content/Characters/Hero.uasset",
                                                "../../../Game/Content/Characters/Hero.uexp"});
    CHECK(stripMountPrefix(pak.files[0]) == "Game/Content/Characters/Hero.uasset");
}

TEST_CASE("malformed or unsupported .pak files are never guessed at") {
    CHECK(parsePak(source("not a pak")).status == ParseStatus::Malformed);
    CHECK(parsePak(source(std::string(300, 'x'))).status == ParseStatus::Malformed);
    // A damaged index fails its SHA-1.
    std::string stub = test::pakStub();
    const auto damaged = parsePak(source(corrupt(stub, 6)));
    CHECK(damaged.status == ParseStatus::Malformed);
    CHECK(damaged.detail.find("SHA-1") != std::string::npos);
    // A footer from a newer, unknown version.
    std::string newer = stub;
    const std::size_t versionAt = newer.size() - 221 + 16 + 1 + 4;
    newer[versionAt] = 12;
    CHECK(parsePak(source(newer)).status == ParseStatus::Unsupported);
    // An index that points outside the file.
    std::string outside = stub;
    outside[newer.size() - 221 + 16 + 1 + 4 + 4] = 0x7f;
    CHECK(parsePak(source(outside)).status == ParseStatus::Malformed);
}

TEST_CASE("an IoStore table of contents parses completely and its chunks verify against the .ucas") {
    const auto set = test::packageSet("/Game/_Dawnwalker/Player/BP_PlayerCharacter", {"/Game/_Dawnwalker/Player/MPC_Player"});
    const auto toc = parseIoStoreToc(source(set.utoc));
    REQUIRE(toc.status == ParseStatus::Parsed);
    CHECK(toc.version == 8);
    CHECK(toc.containerId == 0xe4a7420854dad984ull);
    CHECK(toc.indexed());
    CHECK_FALSE(toc.compressed());
    CHECK_FALSE(toc.encrypted());
    CHECK_FALSE(toc.signedContainer());
    REQUIRE(toc.chunks.size() == 2);
    CHECK(toc.chunks[0].id == 0x7285fde174bd91f8ull);  // the package id of BP_PlayerCharacter
    CHECK(toc.chunks[0].type == static_cast<std::uint8_t>(IoChunkType::ExportBundleData));
    CHECK(toc.chunks[1].type == static_cast<std::uint8_t>(IoChunkType::ContainerHeader));
    CHECK(toc.directoryIndexParsed);
    REQUIRE(toc.files.size() == 1);
    CHECK(toc.files[0].first == "BP_PlayerCharacter.uasset");
    CHECK(toc.casBytesRequired == set.ucas.size());

    MemorySource cas(set.ucas);
    const auto verification = verifyChunks(toc, cas, 1 << 20);
    CHECK(verification.verified == 2);
    CHECK(verification.allVerified());
    const auto header = parseContainerHeader(readChunk(toc, 1, cas, 1 << 20).value());
    REQUIRE(header.status == ParseStatus::Parsed);
    CHECK(header.containerId == toc.containerId);
    CHECK(header.packageIds == std::vector<std::uint64_t>{0x7285fde174bd91f8ull});
    const auto package = parseZenPackage(readChunk(toc, 0, cas, 1 << 20).value());
    REQUIRE(package.status == ParseStatus::Parsed);
    CHECK(package.name == "/Game/_Dawnwalker/Player/BP_PlayerCharacter");
    CHECK(package.layout == "UE 5.3+");
    CHECK(package.cooked());
    CHECK(package.unversioned());
    CHECK(package.importedPackages == std::vector<std::string>{"/Game/_Dawnwalker/Player/MPC_Player"});
}

TEST_CASE("older IoStore versions with 32-byte chunk hashes still verify") {
    const auto files = test::ioStore(0x1111, {{0x2222, 1, std::string(70000, 'z')}}, "Asset.uasset", 7);
    const auto toc = parseIoStoreToc(source(files.utoc));
    REQUIRE(toc.status == ParseStatus::Parsed);
    CHECK(toc.version == 7);
    CHECK(toc.blocks.size() == 2);  // 70000 bytes span two 64 KiB blocks
    MemorySource cas(files.ucas);
    CHECK(verifyChunks(toc, cas, 1 << 20).allVerified());
}

TEST_CASE("malformed and unsupported IoStore tables are rejected, damaged data is detected") {
    const auto set = test::packageSet("/Game/X/Y", {});
    CHECK(parseIoStoreToc(source(set.utoc.substr(0, 100))).status == ParseStatus::Malformed);
    CHECK(parseIoStoreToc(source(set.utoc.substr(0, set.utoc.size() - 3))).status == ParseStatus::Malformed);
    CHECK(parseIoStoreToc(source(corrupt(set.utoc, 0))).status == ParseStatus::Malformed);  // magic
    std::string newer = set.utoc;
    newer[16] = 9;
    const auto unsupported = parseIoStoreToc(source(newer));
    CHECK(unsupported.status == ParseStatus::Unsupported);
    CHECK(unsupported.version == 9);
    // A chunk count whose ids fit but whose offsets and metadata cannot: refused before allocating.
    std::string inflated = set.utoc;
    inflated[24] = 10;
    const auto hostile = parseIoStoreToc(source(inflated));
    CHECK(hostile.status == ParseStatus::Malformed);
    CHECK(hostile.chunks.empty());
    std::string trailing = set.utoc + "junk";
    CHECK(parseIoStoreToc(source(trailing)).status == ParseStatus::Malformed);
    // Tampered container data: every hash is recomputed.
    const auto toc = parseIoStoreToc(source(set.utoc));
    MemorySource damaged(corrupt(set.ucas, 10));
    CHECK(verifyChunks(toc, damaged, 1 << 20).mismatched == 1);
    // The header-only and chunk-id-only modes read no more than they need.
    TocParseOptions header;
    header.headerOnly = true;
    CHECK(parseIoStoreToc(source(set.utoc), header).chunks.empty());
    TocParseOptions ids;
    ids.chunkIdsOnly = true;
    const auto idsOnly = parseIoStoreToc(source(set.utoc), ids);
    CHECK(idsOnly.packageIds == std::vector<std::uint64_t>{packageIdFromName("/Game/X/Y")});
}

TEST_CASE("legacy package summaries report unversioned cooking") {
    test::Bytes head;
    head.u32(kPackageFileTag).i32(-8).i32(0).i32(0).i32(0).i32(0);
    const auto summary = parseLegacyPackageSummary(head.str());
    REQUIRE(summary.status == ParseStatus::Parsed);
    CHECK(summary.unversioned());
    test::Bytes versioned;
    versioned.u32(kPackageFileTag).i32(-7).i32(864).i32(522).i32(0);
    const auto old = parseLegacyPackageSummary(versioned.str());
    REQUIRE(old.status == ParseStatus::Parsed);
    CHECK(old.fileVersionUE4 == 522);
    CHECK_FALSE(old.unversioned());
    CHECK(parseLegacyPackageSummary("MZ...").status == ParseStatus::Malformed);
}
