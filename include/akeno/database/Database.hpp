// SPDX-License-Identifier: GPL-3.0-or-later
// RAII wrapper around one SQLite connection.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "akeno/core/Result.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace akeno::database {

class Statement {
public:
    Statement() = default;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Parameter indices are 1-based, as in SQLite.
    Status bind(int index, std::int64_t value);
    Status bind(int index, std::string_view value);
    Status bindNull(int index);

    // true = a row is available, false = done.
    Result<bool> step();
    Status reset();

    std::int64_t columnInt64(int column) const;
    std::string columnText(int column) const;
    bool columnIsNull(int column) const;

private:
    friend class Database;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    Error lastError(std::string message) const;

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    // Opens (creating if needed) a database file with conservative durability settings:
    // rollback journal (no WAL shared memory), synchronous=FULL, foreign keys on.
    static Result<std::unique_ptr<Database>> open(const std::filesystem::path& file);
    // In-memory database for tests.
    static Result<std::unique_ptr<Database>> openInMemory();

    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Status exec(std::string_view sql);
    Result<Statement> prepare(std::string_view sql);

    // Runs `body` inside BEGIN IMMEDIATE / COMMIT; rolls back if it returns an error.
    Status transaction(const std::function<Status()>& body);

    Result<int> userVersion();
    Status setUserVersion(int version);

    // PRAGMA quick_check; returns "ok" or the first problem found.
    Result<std::string> quickCheck();

    std::int64_t lastInsertRowId() const;
    std::mutex& mutex() noexcept { return mutex_; }
    const std::filesystem::path& file() const noexcept { return file_; }

private:
    Database(sqlite3* handle, std::filesystem::path file) : handle_(handle), file_(std::move(file)) {}
    Error lastError(std::string message) const;

    sqlite3* handle_ = nullptr;
    std::filesystem::path file_;
    std::mutex mutex_;
};

std::string sqliteVersion();

}  // namespace akeno::database
