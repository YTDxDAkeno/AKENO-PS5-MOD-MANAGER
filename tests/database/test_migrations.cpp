// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <array>

#include "TestSupport.hpp"
#include "akeno/database/Migrations.hpp"

using namespace akeno;
using namespace akeno::database;
namespace fs = std::filesystem;

namespace {

int countRows(Database& db, const char* sql) {
    auto stmt = db.prepare(sql);
    REQUIRE(stmt.ok());
    auto row = stmt->step();
    REQUIRE(row.ok());
    REQUIRE(row.value());
    return static_cast<int>(stmt->columnInt64(0));
}

constexpr std::array<Migration, 2> kTestMigrations{{
    {1, "create a", "CREATE TABLE a (x INTEGER);"},
    {2, "create b", "CREATE TABLE b (y INTEGER);"},
}};

}  // namespace

TEST_CASE("built-in migrations apply to a fresh database") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    bool backupCalled = false;
    auto report = migrate(*db.value(), builtinMigrations(), [&](int) -> Result<fs::path> {
        backupCalled = true;
        return fs::path("/unused");
    });
    REQUIRE(report.ok());
    CHECK(report->fromVersion == 0);
    CHECK(report->toVersion == latestSchemaVersion());
    CHECK_FALSE(backupCalled);  // nothing to back up in a new database
    CHECK(db.value()->userVersion().value() == latestSchemaVersion());
    CHECK(countRows(*db.value(), "SELECT COUNT(*) FROM schema_history;") == latestSchemaVersion());
    CHECK(countRows(*db.value(), "SELECT COUNT(*) FROM games;") == 0);
    CHECK(countRows(*db.value(), "SELECT COUNT(*) FROM settings;") == 0);
}

TEST_CASE("migrating twice is a no-op") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    REQUIRE(migrate(*db.value(), builtinMigrations(), {}).ok());
    auto again = migrate(*db.value(), builtinMigrations(), {});
    REQUIRE(again.ok());
    CHECK(again->applied.empty());
}

TEST_CASE("existing data survives an upgrade and a backup is taken first") {
    test::TempDir dir;
    const fs::path file = dir.path() / "akeno.sqlite";
    {
        auto db = Database::open(file);
        REQUIRE(db.ok());
        REQUIRE(migrate(*db.value(), std::span<const Migration>(kTestMigrations.data(), 1), {}).ok());
        REQUIRE(db.value()->exec("INSERT INTO a VALUES (42);").ok());
    }
    auto db = Database::open(file);
    REQUIRE(db.ok());
    int backupFrom = -1;
    auto report = migrate(*db.value(), kTestMigrations, [&](int fromVersion) -> Result<fs::path> {
        backupFrom = fromVersion;
        fs::path target = dir.path() / "backup.sqlite";
        fs::copy_file(file, target);
        return target;
    });
    REQUIRE(report.ok());
    CHECK(backupFrom == 1);
    CHECK(report->toVersion == 2);
    CHECK(fs::exists(dir.path() / "backup.sqlite"));
    CHECK(countRows(*db.value(), "SELECT x FROM a;") == 42);
}

TEST_CASE("a database from a newer version is refused untouched") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    REQUIRE(db.value()->setUserVersion(99).ok());
    auto report = migrate(*db.value(), builtinMigrations(), {});
    REQUIRE_FALSE(report.ok());
    CHECK(report.error().code == ErrorCode::Unsupported);
    CHECK(db.value()->userVersion().value() == 99);
}

TEST_CASE("a failing migration rolls back and keeps the previous version") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    const std::array<Migration, 2> broken{{
        {1, "ok", "CREATE TABLE a (x INTEGER);"},
        {2, "broken", "CREATE TABLE b (y INTEGER); THIS IS NOT SQL;"},
    }};
    auto report = migrate(*db.value(), broken, {});
    REQUIRE_FALSE(report.ok());
    CHECK(db.value()->userVersion().value() == 1);
    // Table b from the failed migration must not exist.
    auto stmt = db.value()->prepare("SELECT COUNT(*) FROM sqlite_master WHERE name = 'b';");
    REQUIRE(stmt.ok());
    REQUIRE(stmt->step().value());
    CHECK(stmt->columnInt64(0) == 0);
}

TEST_CASE("a failed backup stops the upgrade") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    REQUIRE(migrate(*db.value(), std::span<const Migration>(kTestMigrations.data(), 1), {}).ok());
    auto report = migrate(*db.value(), kTestMigrations, [](int) -> Result<fs::path> {
        return makeError(ErrorCode::NoSpace, "disk full");
    });
    REQUIRE_FALSE(report.ok());
    CHECK(db.value()->userVersion().value() == 1);
}

TEST_CASE("inconsistent migration lists are rejected") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    const std::array<Migration, 1> gap{{{2, "starts at two", "CREATE TABLE z (a INTEGER);"}}};
    auto report = migrate(*db.value(), gap, {});
    REQUIRE_FALSE(report.ok());
    CHECK(report.error().code == ErrorCode::Internal);
}

TEST_CASE("quick check reports ok for a healthy database") {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    CHECK(db.value()->quickCheck().value() == "ok");
}
