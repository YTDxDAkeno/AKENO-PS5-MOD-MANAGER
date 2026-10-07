// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/database/Migrations.hpp"

#include <array>

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"

namespace akeno::database {

namespace {

// Migration 1 — Phase 1 (safe game browser).
constexpr const char* kSchemaHistoryTable = R"sql(
CREATE TABLE IF NOT EXISTS schema_history (
    version     INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    applied_at  TEXT NOT NULL
);
)sql";

constexpr const char* kMigration1 = R"sql(
CREATE TABLE settings (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    updated_at  TEXT NOT NULL
);

CREATE TABLE games (
    title_id            TEXT PRIMARY KEY,
    name                TEXT NOT NULL,
    version             TEXT NOT NULL DEFAULT '',
    content_id          TEXT NOT NULL DEFAULT '',
    platform            TEXT NOT NULL DEFAULT 'unknown',
    source_type         TEXT NOT NULL DEFAULT '',
    install_path        TEXT NOT NULL DEFAULT '',
    runtime_path        TEXT NOT NULL DEFAULT '',
    previous_version    TEXT,
    version_changed_at  TEXT,
    first_seen_at       TEXT NOT NULL,
    last_seen_at        TEXT NOT NULL
);
)sql";

// Migration 2 — Phase 3 (download engine). One row per download; the file names in
// downloads/ are derived from `id` only.
constexpr const char* kMigration2 = R"sql(
CREATE TABLE downloads (
    id               TEXT PRIMARY KEY,
    provider_id      TEXT NOT NULL,
    mod_id           TEXT NOT NULL,
    name             TEXT NOT NULL,
    mod_version      TEXT NOT NULL DEFAULT '',
    game_title_id    TEXT NOT NULL DEFAULT '',
    game_version     TEXT NOT NULL DEFAULT '',
    compatibility    TEXT NOT NULL DEFAULT '',
    url              TEXT NOT NULL,
    expected_size    INTEGER NOT NULL,
    expected_sha256  TEXT NOT NULL,
    format           TEXT NOT NULL,
    state            TEXT NOT NULL,
    bytes_done       INTEGER NOT NULL DEFAULT 0,
    attempts         INTEGER NOT NULL DEFAULT 0,
    error            TEXT NOT NULL DEFAULT '',
    created_at       TEXT NOT NULL,
    updated_at       TEXT NOT NULL,
    completed_at     TEXT NOT NULL DEFAULT ''
);
CREATE INDEX downloads_by_mod ON downloads(provider_id, mod_id, mod_version);
)sql";

constexpr std::array<Migration, 2> kMigrations{{
    {1, "phase1: settings and game snapshot", kMigration1},
    {2, "phase3: downloads", kMigration2},
}};

}  // namespace

std::span<const Migration> builtinMigrations() { return kMigrations; }

int latestSchemaVersion() { return kMigrations.back().version; }

Result<MigrationReport> migrate(Database& db, std::span<const Migration> migrations, const BackupFunction& backup) {
    MigrationReport report;
    auto current = db.userVersion();
    if (!current) {
        return std::move(current).error();
    }
    report.fromVersion = current.value();
    report.toVersion = current.value();

    int latest = 0;
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version != static_cast<int>(i) + 1) {
            return makeError(ErrorCode::Internal, "The built-in database migrations are inconsistent.",
                             strings::concat("migration index ", i, " has version ", migrations[i].version));
        }
        latest = migrations[i].version;
    }
    if (current.value() > latest) {
        return makeError(ErrorCode::Unsupported,
                         "The database was created by a newer version of Akeno Mod Manager. It was left "
                         "unchanged. Please update the application.",
                         strings::concat("database version ", current.value(), ", supported ", latest));
    }
    if (current.value() == latest) {
        return report;
    }
    if (current.value() > 0 && backup) {
        auto backupFile = backup(current.value());
        if (!backupFile) {
            return makeError(ErrorCode::IoError,
                             "Could not back up the database before upgrading it. Nothing was changed.",
                             backupFile.error().describe());
        }
        report.backupFile = std::move(backupFile).value();
    }

    for (const Migration& migration : migrations) {
        if (migration.version <= current.value()) {
            continue;
        }
        Status applied = db.transaction([&]() -> Status {
            AKENO_TRY(db.exec(kSchemaHistoryTable));
            AKENO_TRY(db.exec(migration.sql));
            auto insert = db.prepare("INSERT INTO schema_history(version, name, applied_at) VALUES (?, ?, ?);");
            if (!insert) {
                return std::move(insert).error();
            }
            AKENO_TRY(insert->bind(1, static_cast<std::int64_t>(migration.version)));
            AKENO_TRY(insert->bind(2, std::string_view(migration.name)));
            AKENO_TRY(insert->bind(3, strings::utcTimestamp()));
            auto done = insert->step();
            if (!done) {
                return std::move(done).error();
            }
            return db.setUserVersion(migration.version);
        });
        if (!applied) {
            Error error = std::move(applied).error();
            error.message = strings::concat("Database upgrade to version ", migration.version,
                                            " failed. The previous version was kept.");
            return error;
        }
        logging::logger().info("database", strings::concat("applied migration ", migration.version, ": ",
                                                            migration.name));
        report.applied.emplace_back(migration.name);
        report.toVersion = migration.version;
    }
    return report;
}

}  // namespace akeno::database
