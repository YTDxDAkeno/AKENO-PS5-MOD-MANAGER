// SPDX-License-Identifier: GPL-3.0-or-later
// Hostile and normal archives are written with libarchive's writer, then read back through
// SecureExtractor.
#include "Doctest.hpp"

#include <optional>

#include "ArchiveTestSupport.hpp"
#include "TestSupport.hpp"
#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/security/Sha256.hpp"

using namespace akeno;
using namespace akeno::archives;
using test::EntrySpec;
using test::writeArchive;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    test::TempDir dir;
    fs::path root = dir.path() / "data";
    fs::path staging = root / "staging";
    std::unique_ptr<security::SafeFs> fs;

    Fixture() {
        fs::create_directories(staging);
        fs = std::make_unique<security::SafeFs>(security::WriteGuard::create({root}).value());
    }
    fs::path archive(const std::string& name) const { return dir.path() / name; }
};

std::string readFile(const fs::path& path) { return security::readFileBounded(path, 64 * 1024 * 1024).value(); }

std::string refusal(const Fixture& f, const std::vector<EntrySpec>& entries,
                    mods::ArchiveFormat format = mods::ArchiveFormat::Tar, ExtractionLimits limits = {}) {
    const fs::path file = f.archive("hostile");
    REQUIRE(writeArchive(file, format, entries));
    SecureExtractor extractor(*f.fs, limits);
    auto result = extractor.extract(file, format, f.staging / "op", nullptr);
    REQUIRE_FALSE(result.ok());
    CHECK_FALSE(fs::exists(f.staging / "op"));  // nothing is left behind
    fs::remove(file);
    return result.error().message;
}

}  // namespace

TEST_CASE("entry names are normalised or refused") {
    CHECK(normalizeEntryPath("mod/Content/Paks/a.pak").value() == "mod/Content/Paks/a.pak");
    CHECK(normalizeEntryPath("folder/").value() == "folder");
    CHECK(normalizeEntryPath("Ünïcødé/文件.txt").value() == "Ünïcødé/文件.txt");
    for (const char* bad : {"", "/etc/passwd", "../evil", "a/../../evil", "a/./b", "./a", "a//b", "C:/x", "a\\..\\b",
                            "a\x01" "b", "/", "a\x7f"}) {
        CAPTURE(bad);
        CHECK_FALSE(normalizeEntryPath(bad).ok());
    }
    CHECK_FALSE(normalizeEntryPath(std::string("bad\xff\xfe.txt")).ok());        // not UTF-8
    CHECK_FALSE(normalizeEntryPath(std::string("\xc0\xaf" "etc")).ok());         // overlong "/"
    CHECK_FALSE(normalizeEntryPath(std::string("\xed\xa0\x80.txt")).ok());       // surrogate
    CHECK_FALSE(normalizeEntryPath(std::string(300, 'a')).ok());                 // component too long
    CHECK_FALSE(normalizeEntryPath(std::string(2000, 'a')).ok());                // path too long
    ExtractionLimits shallow;
    shallow.maxDepth = 3;
    CHECK(normalizeEntryPath("a/b/c", shallow).ok());
    CHECK_FALSE(normalizeEntryPath("a/b/c/d", shallow).ok());
}

TEST_CASE("zip, tar.gz and 7z archives are extracted with checksums") {
    Fixture f;
    const std::vector<EntrySpec> entries{
        {"mod/", "", AE_IFDIR, "", ""},
        {"mod/Content/Paks/~mods/outfit.pak", std::string(5000, 'p'), AE_IFREG, "", ""},
        {"mod/config.ini", "[outfit]\ncolour=crimson\n", AE_IFREG, "", ""},
        {"README.txt", "Read me", AE_IFREG, "", ""},
        {"empty.txt", "", AE_IFREG, "", ""},
    };
    for (auto format : {mods::ArchiveFormat::Zip, mods::ArchiveFormat::TarGz, mods::ArchiveFormat::Tar,
                        mods::ArchiveFormat::SevenZip}) {
        CAPTURE(mods::toString(format));
        const fs::path file = f.archive(std::string("good.") + std::string(mods::toString(format)));
        REQUIRE(writeArchive(file, format, entries));
        SecureExtractor extractor(*f.fs);
        auto listing = extractor.inspect(file, format);
        REQUIRE(listing.ok());
        CHECK(listing->fileCount == 4);
        std::uint64_t expectedTotal = 0;
        for (const auto& spec : entries) expectedTotal += spec.data.size();
        CHECK(listing->totalBytes == expectedTotal);

        const fs::path destination = f.staging / ("op-" + std::string(mods::toString(format)));
        std::uint64_t lastProgress = 0;
        auto tree = extractor.extract(file, format, destination, nullptr,
                                      [&](std::uint64_t done, std::uint64_t) { lastProgress = done; });
        REQUIRE(tree.ok());
        CHECK(lastProgress == expectedTotal);
        REQUIRE(tree->files.size() == 4);
        CHECK(readFile(destination / "mod/Content/Paks/~mods/outfit.pak") == std::string(5000, 'p'));
        CHECK(readFile(destination / "mod/config.ini") == "[outfit]\ncolour=crimson\n");
        for (const auto& extracted : tree->files) {
            CHECK(extracted.sha256 == security::sha256Hex(readFile(destination / extracted.path)));
        }
        const auto pak = std::find_if(tree->files.begin(), tree->files.end(),
                                      [](const ExtractedFile& e) { return e.path == "mod/Content/Paks/~mods/outfit.pak"; });
        REQUIRE(pak != tree->files.end());
        CHECK(pak->head == std::string(kHeadBytes, 'p'));

        // The destination must be new.
        auto again = extractor.extract(file, format, destination, nullptr);
        REQUIRE_FALSE(again.ok());
        CHECK(again.error().code == ErrorCode::AlreadyExists);
    }
}

TEST_CASE("path tricks are refused and leave nothing behind") {
    Fixture f;
    CHECK(refusal(f, {{"ok.txt", "x", AE_IFREG, "", ""}, {"../evil.txt", "x", AE_IFREG, "", ""}}).find("leave its folder") !=
          std::string::npos);
    CHECK(refusal(f, {{"/abs/evil.txt", "x", AE_IFREG, "", ""}}).find("absolute") != std::string::npos);
    CHECK(refusal(f, {{"a/../../evil.txt", "x", AE_IFREG, "", ""}}, mods::ArchiveFormat::Zip).find("leave") !=
          std::string::npos);
    // Tar keeps the backslash; libarchive's zip reader turns it into "/" first. Both are refused.
    CHECK(refusal(f, {{"dir\\evil.txt", "x", AE_IFREG, "", ""}}).find("backslash") != std::string::npos);
    CHECK(refusal(f, {{"dir\\..\\evil.txt", "x", AE_IFREG, "", ""}}, mods::ArchiveFormat::Zip).find("leave") !=
          std::string::npos);
    CHECK(refusal(f, {{"C:/evil.txt", "x", AE_IFREG, "", ""}}, mods::ArchiveFormat::Zip).find("colon") !=
          std::string::npos);
    CHECK_FALSE(fs::exists(f.dir.path() / "evil.txt"));
    CHECK_FALSE(fs::exists("/abs/evil.txt"));
}

TEST_CASE("links and special files are refused") {
    Fixture f;
    CHECK(refusal(f, {{"link", "", AE_IFLNK, "/etc/passwd", ""}}).find("symbolic link") != std::string::npos);
    CHECK(refusal(f, {{"target.txt", "x", AE_IFREG, "", ""}, {"hard", "", AE_IFREG, "", "target.txt"}}).find("hard link") !=
          std::string::npos);
    CHECK(refusal(f, {{"dev", "", AE_IFCHR, "", ""}}).find("device") != std::string::npos);
    CHECK(refusal(f, {{"fifo", "", AE_IFIFO, "", ""}}).find("device, pipe") != std::string::npos);
    CHECK(refusal(f, {{"link", "", AE_IFLNK, "../outside", ""}}, mods::ArchiveFormat::Zip).find("symbolic link") !=
          std::string::npos);
}

TEST_CASE("duplicates and file/folder clashes are refused") {
    Fixture f;
    CHECK(refusal(f, {{"a.txt", "1", AE_IFREG, "", ""}, {"a.txt", "2", AE_IFREG, "", ""}}).find("same name twice") !=
          std::string::npos);
    CHECK(refusal(f, {{"a", "file", AE_IFREG, "", ""}, {"a/b.txt", "x", AE_IFREG, "", ""}}).find("used as a folder") !=
          std::string::npos);
}

TEST_CASE("limits on count, size, depth and compression ratio") {
    Fixture f;
    ExtractionLimits few;
    few.maxEntries = 2;
    CHECK(refusal(f, {{"1", "a", AE_IFREG, "", ""}, {"2", "b", AE_IFREG, "", ""}, {"3", "c", AE_IFREG, "", ""}},
                  mods::ArchiveFormat::Tar, few)
              .find("more than 2 entries") != std::string::npos);
    ExtractionLimits small;
    small.maxTotalBytes = 100;
    CHECK(refusal(f, {{"1", std::string(60, 'a'), AE_IFREG, "", ""}, {"2", std::string(60, 'b'), AE_IFREG, "", ""}},
                  mods::ArchiveFormat::Tar, small)
              .find("unpack to more than") != std::string::npos);
    ExtractionLimits perFile;
    perFile.maxEntryBytes = 10;
    CHECK(refusal(f, {{"big", std::string(11, 'a'), AE_IFREG, "", ""}}, mods::ArchiveFormat::Tar, perFile)
              .find("larger than") != std::string::npos);
    ExtractionLimits shallow;
    shallow.maxDepth = 2;
    CHECK(refusal(f, {{"a/b/c.txt", "x", AE_IFREG, "", ""}}, mods::ArchiveFormat::Tar, shallow).find("nested") !=
          std::string::npos);
    ExtractionLimits bomb;
    bomb.ratioCheckFrom = 1024 * 1024;
    bomb.maxCompressionRatio = 100;
    CHECK(refusal(f, {{"zeros.bin", std::string(2 * 1024 * 1024, '\0'), AE_IFREG, "", ""}}, mods::ArchiveFormat::Zip, bomb)
              .find("archive bomb") != std::string::npos);
}

TEST_CASE("damaged, mislabelled and encrypted archives") {
    Fixture f;
    const fs::path tar = f.archive("actually.tar");
    REQUIRE(writeArchive(tar, mods::ArchiveFormat::Tar, {{"a.txt", "hello", AE_IFREG, "", ""}}));
    SecureExtractor extractor(*f.fs);
    auto mislabelled = extractor.extract(tar, mods::ArchiveFormat::Zip, f.staging / "op1");
    REQUIRE_FALSE(mislabelled.ok());
    CHECK(mislabelled.error().message == "The file is not a valid zip archive.");
    CHECK_FALSE(fs::exists(f.staging / "op1"));

    const fs::path zip = f.archive("truncated.zip");
    REQUIRE(writeArchive(zip, mods::ArchiveFormat::Zip,
                         {{"a.bin", security::sha256Hex("seed") + std::string(20000, 'q'), AE_IFREG, "", ""}}));
    fs::resize_file(zip, fs::file_size(zip) / 2);
    auto truncated = extractor.extract(zip, mods::ArchiveFormat::Zip, f.staging / "op2");
    REQUIRE_FALSE(truncated.ok());
    CHECK_FALSE(fs::exists(f.staging / "op2"));

    const fs::path garbage = f.archive("garbage.7z");
    test::writeText(garbage, "this is not an archive at all");
    CHECK_FALSE(extractor.extract(garbage, mods::ArchiveFormat::SevenZip, f.staging / "op3").ok());

    const fs::path encrypted = f.archive("encrypted.zip");
    if (writeArchive(encrypted, mods::ArchiveFormat::Zip, {{"secret.txt", "hidden", AE_IFREG, "", ""}}, true)) {
        auto result = extractor.extract(encrypted, mods::ArchiveFormat::Zip, f.staging / "op4");
        REQUIRE_FALSE(result.ok());
        CHECK(result.error().message.find("password") != std::string::npos);
        CHECK_FALSE(fs::exists(f.staging / "op4"));
    } else {
        MESSAGE("this libarchive cannot write encrypted zip files; encryption test skipped");
    }

    auto unsupported = extractor.extract(tar, mods::ArchiveFormat::Unknown, f.staging / "op5");
    CHECK_FALSE(unsupported.ok());
}

TEST_CASE("extraction can be cancelled and cleans up") {
    Fixture f;
    const fs::path zip = f.archive("cancel.zip");
    REQUIRE(writeArchive(zip, mods::ArchiveFormat::Zip, {{"a.bin", std::string(100000, 'a'), AE_IFREG, "", ""}}));
    CancellationToken cancel;
    cancel.cancel();
    SecureExtractor extractor(*f.fs);
    auto result = extractor.extract(zip, mods::ArchiveFormat::Zip, f.staging / "op", &cancel);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(fs::exists(f.staging / "op"));
}

TEST_CASE("a symlinked archive file or destination is not used") {
    Fixture f;
    const fs::path zip = f.archive("real.zip");
    REQUIRE(writeArchive(zip, mods::ArchiveFormat::Zip, {{"a.txt", "x", AE_IFREG, "", ""}}));
    const fs::path link = f.archive("link.zip");
    fs::create_symlink(zip, link);
    SecureExtractor extractor(*f.fs);
    CHECK_FALSE(extractor.extract(link, mods::ArchiveFormat::Zip, f.staging / "op1").ok());

    // A destination outside the application directory is refused by the write guard.
    CHECK_FALSE(extractor.extract(zip, mods::ArchiveFormat::Zip, f.dir.path() / "outside").ok());
    CHECK_FALSE(fs::exists(f.dir.path() / "outside"));
}
