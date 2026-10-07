// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <unistd.h>

#include <sys/stat.h>

#include "TestSupport.hpp"
#include "akeno/security/SafeFs.hpp"

using namespace akeno;
using namespace akeno::security;
namespace fs = std::filesystem;

namespace {

SafeFs makeFs(const fs::path& root) { return SafeFs(WriteGuard::create({root}).value()); }

}  // namespace

TEST_CASE("writeFileAtomic writes, replaces and leaves no temporary file") {
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path());
    const fs::path file = dir.path() / "state.json";
    REQUIRE(safe.writeFileAtomic(file, "one").ok());
    REQUIRE(safe.writeFileAtomic(file, "two").ok());
    CHECK(readFileBounded(file, 100).value() == "two");
    int entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir.path())) ++entries;
    CHECK(entries == 1);
}

TEST_CASE("writeFileAtomic refuses targets outside the root") {
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path() / "root");
    auto result = safe.writeFileAtomic(dir.path() / "elsewhere.txt", "x");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::SafetyViolation);
    CHECK_FALSE(fs::exists(dir.path() / "elsewhere.txt"));
}

TEST_CASE("createDirectories builds nested directories inside the root") {
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path());
    REQUIRE(safe.createDirectories(dir.path() / "a" / "b" / "c").ok());
    CHECK(fs::is_directory(dir.path() / "a" / "b" / "c"));
}

TEST_CASE("removeTree removes a tree but never the root itself") {
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path());
    fs::create_directories(dir.path() / "staging" / "x");
    test::writeText(dir.path() / "staging" / "x" / "f", "data");
    REQUIRE(safe.removeTree(dir.path() / "staging" / "x").ok());
    CHECK_FALSE(fs::exists(dir.path() / "staging" / "x"));

    auto rootRemoval = safe.removeTree(dir.path());
    REQUIRE_FALSE(rootRemoval.ok());
    CHECK(rootRemoval.error().code == ErrorCode::SafetyViolation);
    CHECK(fs::exists(dir.path()));
}

TEST_CASE("removeTree does not follow symlinks out of the root") {
    test::TempDir outer;
    fs::create_directories(outer.path() / "root" / "staging");
    fs::create_directories(outer.path() / "precious");
    test::writeText(outer.path() / "precious" / "keep.txt", "keep");
    fs::create_directory_symlink(outer.path() / "precious", outer.path() / "root" / "staging" / "link");
    SafeFs safe = makeFs(outer.path() / "root");
    REQUIRE(safe.removeTree(outer.path() / "root" / "staging").ok());
    CHECK(fs::exists(outer.path() / "precious" / "keep.txt"));
}

TEST_CASE("removeTree deletes deep trees, hidden files and missing paths") {
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path());
    fs::path deep = dir.path() / "staging" / "check";
    for (int i = 0; i < 40; ++i) deep /= "d";
    fs::create_directories(deep);
    test::writeText(deep / ".hidden", "x");
    test::writeText(dir.path() / "staging" / "check" / "top.txt", "x");
    fs::create_symlink("/nonexistent", dir.path() / "staging" / "check" / "dangling");
    REQUIRE(safe.removeTree(dir.path() / "staging" / "check").ok());
    CHECK_FALSE(fs::exists(fs::symlink_status(dir.path() / "staging" / "check")));
    CHECK(fs::is_empty(dir.path() / "staging"));
    CHECK(safe.removeTree(dir.path() / "staging" / "never-there").ok());
}

TEST_CASE("removeTree reports a folder it cannot delete") {
    if (::geteuid() == 0) return;  // root ignores directory permissions
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path());
    fs::create_directories(dir.path() / "staging" / "locked" / "inner");
    test::writeText(dir.path() / "staging" / "locked" / "inner" / "f", "x");
    fs::permissions(dir.path() / "staging" / "locked" / "inner", fs::perms::owner_read | fs::perms::owner_exec);
    auto removed = safe.removeTree(dir.path() / "staging" / "locked");
    fs::permissions(dir.path() / "staging" / "locked" / "inner", fs::perms::owner_all);
    CHECK_FALSE(removed.ok());
}

TEST_CASE("hard links can be created inside the root") {
    test::TempDir dir;
    SafeFs safe = makeFs(dir.path());
    REQUIRE(safe.writeFileAtomic(dir.path() / "a", "x").ok());
    REQUIRE(safe.createHardLink(dir.path() / "a", dir.path() / "b").ok());
    struct stat info {};
    REQUIRE(::stat((dir.path() / "b").c_str(), &info) == 0);
    CHECK(info.st_nlink == 2);
}

TEST_CASE("readFileBounded refuses large files and non-regular files") {
    test::TempDir dir;
    test::writeText(dir.path() / "big", std::string(1000, 'x'));
    auto tooBig = readFileBounded(dir.path() / "big", 100);
    REQUIRE_FALSE(tooBig.ok());
    CHECK(tooBig.error().code == ErrorCode::ResponseTooLarge);
    CHECK_FALSE(readFileBounded(dir.path(), 100).ok());
    CHECK(readFileBounded(dir.path() / "missing", 100).error().code == ErrorCode::NotFound);
}

TEST_CASE("queryStorageSpace reports sensible numbers") {
    test::TempDir dir;
    auto space = queryStorageSpace(dir.path());
    REQUIRE(space.ok());
    CHECK(space->totalBytes > 0);
    CHECK(space->availableBytes <= space->totalBytes);
}
