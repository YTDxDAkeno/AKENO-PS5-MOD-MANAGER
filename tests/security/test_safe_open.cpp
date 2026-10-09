// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <unistd.h>

#include "TestSupport.hpp"
#include "akeno/security/SafeOpen.hpp"

using namespace akeno;
namespace fs = std::filesystem;

TEST_CASE("descriptors are duplicated with dup(): both copies read the same file independently") {
    test::TempDir dir;
    test::writeText(dir.path() / "a.txt", "hello");
    security::UniqueFd file = security::openNoFollow(dir.path() / "a.txt", false);
    REQUIRE(file.valid());
    security::UniqueFd copy(security::duplicateDescriptor(file.get()));
    REQUIRE(copy.valid());
    CHECK(copy.get() != file.get());
    char buffer[5] = {};
    CHECK(::pread(copy.get(), buffer, sizeof buffer, 0) == 5);
    CHECK(std::string(buffer, 5) == "hello");
    copy = security::UniqueFd();  // closing the copy leaves the original usable
    CHECK(::pread(file.get(), buffer, 1, 0) == 1);
    CHECK(security::duplicateDescriptor(-1) == -1);
}

TEST_CASE("files below an opened folder are opened without following links") {
    test::TempDir dir;
    fs::create_directories(dir.path() / "Better Carry Weight x10");
    test::writeText(dir.path() / "Better Carry Weight x10" / "00000000_X_P.pak", "pak");
    security::UniqueFd root = security::openNoFollow(dir.path(), true);
    REQUIRE(root.valid());
    security::UniqueFd file = security::openBelow(root.get(), "Better Carry Weight x10/00000000_X_P.pak", false);
    CHECK(file.valid());
    CHECK(root.valid());  // the folder descriptor stays open for the next file
    CHECK(security::openBelow(root.get(), "Better Carry Weight x10/00000000_X_P.pak", false).valid());
    fs::create_symlink(dir.path() / "Better Carry Weight x10", dir.path() / "link");
    CHECK_FALSE(security::openBelow(root.get(), "link/00000000_X_P.pak", false).valid());
    CHECK_FALSE(security::openBelow(root.get(), "../x", false).valid());
}

TEST_CASE("the folder access probe reports success, or the failing step with errno") {
    test::TempDir dir;
    test::writeText(dir.path() / "probe.txt", "x");
    CHECK(security::probeFolderAccess(dir.path(), "probe.txt").empty());
    const std::string missing = security::probeFolderAccess(dir.path(), "missing.txt");
    CHECK(missing.find("missing.txt") != std::string::npos);
    CHECK(missing.find("ENOENT") != std::string::npos);
    CHECK(security::probeFolderAccess(dir.path() / "nowhere", "probe.txt").find("ENOENT") != std::string::npos);
}
