// SPDX-License-Identifier: GPL-3.0-or-later
// Forward-only schema migrations. The database is never deleted because the schema changed.
#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/database/Database.hpp"

namespace akeno::database {

struct Migration {
    int version;        // strictly increasing, starting at 1
    const char* name;   // short description recorded in schema_history
    const char* sql;    // executed inside the migration's transaction
};

// The migrations shipped with this build.
std::span<const Migration> builtinMigrations();
int latestSchemaVersion();

struct MigrationReport {
    int fromVersion = 0;
    int toVersion = 0;
    std::vector<std::string> applied;
    std::filesystem::path backupFile;  // empty if no backup was needed
};

// Called before the first pending migration of an existing database (version > 0) to copy the
// file somewhere safe. Returns the path of the copy.
using BackupFunction = std::function<Result<std::filesystem::path>(int fromVersion)>;

// Applies pending migrations, each in its own transaction, recording them in schema_history
// and PRAGMA user_version. A database whose version is newer than this build knows is
// refused untouched.
Result<MigrationReport> migrate(Database& db, std::span<const Migration> migrations, const BackupFunction& backup);

}  // namespace akeno::database
