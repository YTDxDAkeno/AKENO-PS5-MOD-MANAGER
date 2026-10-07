// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/security/PathGuard.hpp"
#include "akeno/security/SafeName.hpp"

using namespace akeno;
using namespace akeno::security;
namespace fs = std::filesystem;

TEST_CASE("normalizeAbsolute collapses separators and dots") {
    CHECK(normalizeAbsolute("/data//akeno/./mods/").value() == fs::path("/data/akeno/mods"));
    CHECK(normalizeAbsolute("/").value() == fs::path("/"));
    CHECK(normalizeAbsolute("///").value() == fs::path("/"));
}

TEST_CASE("normalizeAbsolute rejects relative, parent and NUL paths") {
    CHECK_FALSE(normalizeAbsolute("relative/path").ok());
    CHECK_FALSE(normalizeAbsolute("").ok());
    CHECK_FALSE(normalizeAbsolute("/data/../system").ok());
    CHECK_FALSE(normalizeAbsolute("/data/akeno/..").ok());
    CHECK_FALSE(normalizeAbsolute(std::string("/data/a\0b", 9)).ok());
    CHECK_FALSE(normalizeAbsolute("/" + std::string(2000, 'a')).ok());
}

TEST_CASE("isWithin respects component boundaries") {
    CHECK(isWithin("/data/akeno", "/data/akeno"));
    CHECK(isWithin("/data/akeno", "/data/akeno/mods/x"));
    CHECK_FALSE(isWithin("/data/akeno", "/data/akeno-evil/x"));
    CHECK_FALSE(isWithin("/data/akeno", "/data"));
    CHECK(isWithin("/", "/anything"));
}

TEST_CASE("system locations are always forbidden") {
    for (const char* path : {"/system/common/lib", "/system_ex/app/PPSA01234/eboot.bin", "/preinst/x", "/preinst2",
                             "/update/PS5UPDATE.PUP", "/dev/da0", "/user/app/PPSA01234/app.pkg",
                             "/mnt/sandbox/PPSA01234_000/app0", "/data/shadowmount/config.ini",
                             "/user/data/shadowmount/x", "/"}) {
        CAPTURE(path);
        CHECK(isForbiddenPath(normalizeAbsolute(path).value()));
    }
    CHECK_FALSE(isForbiddenPath("/data/akeno-mod-manager/mods"));
    CHECK_FALSE(isForbiddenPath("/system_extra_not_really"));  // boundary check
}

TEST_CASE("WriteGuard refuses forbidden roots") {
    CHECK_FALSE(WriteGuard::create({"/system_ex/app"}).ok());
    CHECK_FALSE(WriteGuard::create({"/"}).ok());
    CHECK_FALSE(WriteGuard::create({}).ok());
    CHECK_FALSE(WriteGuard::create({"relative"}).ok());
}

TEST_CASE("WriteGuard allows writes only inside its roots") {
    test::TempDir dir;
    auto guard = WriteGuard::create({dir.path() / "app"});
    REQUIRE(guard.ok());
    CHECK(guard->checkWritable(dir.path() / "app" / "mods" / "x.bin").ok());
    CHECK(guard->checkWritable(dir.path() / "app").ok());

    auto outside = guard->checkWritable(dir.path() / "other" / "x");
    REQUIRE_FALSE(outside.ok());
    CHECK(outside.error().code == ErrorCode::SafetyViolation);

    CHECK_FALSE(guard->checkWritable(dir.path() / "app-evil").ok());
    CHECK_FALSE(guard->checkWritable("/system_ex/app/PPSA01234/eboot.bin").ok());
    CHECK_FALSE(guard->checkWritable("relative/file").ok());
}

TEST_CASE("WriteGuard refuses to write through a symlink inside its root") {
    test::TempDir dir;
    fs::create_directories(dir.path() / "app");
    fs::create_directories(dir.path() / "outside");
    fs::create_directory_symlink(dir.path() / "outside", dir.path() / "app" / "link");
    auto guard = WriteGuard::create({dir.path() / "app"});
    REQUIRE(guard.ok());
    auto through = guard->checkWritable(dir.path() / "app" / "link" / "file.txt");
    REQUIRE_FALSE(through.ok());
    CHECK(through.error().code == ErrorCode::SafetyViolation);
    // The symlink itself is also not a writable target.
    CHECK_FALSE(guard->checkWritable(dir.path() / "app" / "link").ok());
}

TEST_CASE("WriteGuard resolves a symlinked root once, like /data -> /user/data") {
    test::TempDir dir;
    fs::create_directories(dir.path() / "user" / "data");
    fs::create_directory_symlink(dir.path() / "user" / "data", dir.path() / "data");
    auto guard = WriteGuard::create({dir.path() / "data" / "akeno"});
    REQUIRE(guard.ok());
    CHECK(guard->roots().front() == dir.path() / "user" / "data" / "akeno");
    CHECK(guard->checkWritable(dir.path() / "user" / "data" / "akeno" / "x").ok());
}

TEST_CASE("safe file names") {
    CHECK(isSafeFileComponent("PPSA01234-1.010.png"));
    CHECK_FALSE(isSafeFileComponent(""));
    CHECK_FALSE(isSafeFileComponent("."));
    CHECK_FALSE(isSafeFileComponent(".."));
    CHECK_FALSE(isSafeFileComponent(".hidden"));
    CHECK_FALSE(isSafeFileComponent("a/b"));
    CHECK_FALSE(isSafeFileComponent("a\\b"));
    CHECK_FALSE(isSafeFileComponent(std::string(200, 'a')));
    CHECK(toSafeFileComponent("../../etc/passwd") == "_.._etc_passwd");
    CHECK(toSafeFileComponent("Outfit Pack (v2).zip") == "Outfit_Pack__v2_.zip");
    CHECK(toSafeFileComponent("...", "fallback") == "fallback");
    CHECK(toSafeFileComponent("", "fallback") == "fallback");
    CHECK(toSafeFileComponent(std::string(300, 'x')).size() == 128);
}
