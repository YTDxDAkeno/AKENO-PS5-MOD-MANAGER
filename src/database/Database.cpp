// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/database/Database.hpp"

#include <sqlite3.h>

#include "akeno/core/Strings.hpp"

namespace akeno::database {

namespace {

Error sqliteError(sqlite3* db, std::string message) {
    std::string detail = db != nullptr ? sqlite3_errmsg(db) : "no connection";
    if (db != nullptr) {
        detail += strings::concat(" (", sqlite3_extended_errcode(db), ")");
    }
    return makeError(ErrorCode::Database, std::move(message), std::move(detail));
}

}  // namespace

std::string sqliteVersion() { return std::string("SQLite ") + sqlite3_libversion(); }

// ---------------------------------------------------------------- Statement

Statement::~Statement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

Error Statement::lastError(std::string message) const { return sqliteError(db_, std::move(message)); }

Status Statement::bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
        return lastError("Could not bind a database value.");
    }
    return {};
}

Status Statement::bind(int index, std::string_view value) {
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        return lastError("Could not bind a database value.");
    }
    return {};
}

Status Statement::bindNull(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
        return lastError("Could not bind a database value.");
    }
    return {};
}

Result<bool> Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    return lastError("A database query failed.");
}

Status Statement::reset() {
    sqlite3_reset(stmt_);
    if (sqlite3_clear_bindings(stmt_) != SQLITE_OK) {
        return lastError("Could not reset a database query.");
    }
    return {};
}

std::int64_t Statement::columnInt64(int column) const { return sqlite3_column_int64(stmt_, column); }

std::string Statement::columnText(int column) const {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    int bytes = sqlite3_column_bytes(stmt_, column);
    if (text == nullptr) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

bool Statement::columnIsNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

// ---------------------------------------------------------------- Database

namespace {

Result<std::unique_ptr<Database>> configure(std::unique_ptr<Database> db, bool fileBacked) {
    // Rollback journal instead of WAL: WAL needs shared-memory mappings whose behaviour on the
    // console file system is unverified. FULL sync trades speed for crash safety.
    if (fileBacked) {
        AKENO_TRY(db->exec("PRAGMA journal_mode=DELETE;"));
    }
    AKENO_TRY(db->exec("PRAGMA synchronous=FULL;"));
    AKENO_TRY(db->exec("PRAGMA foreign_keys=ON;"));
    return db;
}

}  // namespace

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& file) {
    sqlite3* handle = nullptr;
    int rc = sqlite3_open_v2(file.c_str(), &handle,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
                                 SQLITE_OPEN_NOFOLLOW,
                             nullptr);
    if (rc != SQLITE_OK) {
        Error error = sqliteError(handle, "Could not open the database.");
        error.detail += " " + file.string();
        sqlite3_close(handle);
        return error;
    }
    sqlite3_busy_timeout(handle, 5000);
    return configure(std::unique_ptr<Database>(new Database(handle, file)), true);
}

Result<std::unique_ptr<Database>> Database::openInMemory() {
    sqlite3* handle = nullptr;
    if (sqlite3_open_v2(":memory:", &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        Error error = sqliteError(handle, "Could not open an in-memory database.");
        sqlite3_close(handle);
        return error;
    }
    return configure(std::unique_ptr<Database>(new Database(handle, ":memory:")), false);
}

Database::~Database() {
    if (handle_ != nullptr) {
        sqlite3_close_v2(handle_);
    }
}

Error Database::lastError(std::string message) const { return sqliteError(handle_, std::move(message)); }

Status Database::exec(std::string_view sql) {
    std::string text(sql);
    char* message = nullptr;
    if (sqlite3_exec(handle_, text.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        Error error = lastError("A database command failed.");
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return error;
    }
    return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
        return lastError("Could not prepare a database query.");
    }
    return Statement(handle_, stmt);
}

Status Database::transaction(const std::function<Status()>& body) {
    AKENO_TRY(exec("BEGIN IMMEDIATE;"));
    Status result = body();
    if (!result) {
        (void)exec("ROLLBACK;");
        return result;
    }
    auto committed = exec("COMMIT;");
    if (!committed) {
        (void)exec("ROLLBACK;");
        return committed;
    }
    return {};
}

Result<int> Database::userVersion() {
    auto stmt = prepare("PRAGMA user_version;");
    if (!stmt) {
        return std::move(stmt).error();
    }
    auto row = stmt->step();
    if (!row) {
        return std::move(row).error();
    }
    if (!row.value()) {
        return makeError(ErrorCode::Database, "Could not read the database version.");
    }
    return static_cast<int>(stmt->columnInt64(0));
}

Status Database::setUserVersion(int version) {
    return exec(strings::concat("PRAGMA user_version = ", version, ";"));
}

Result<std::string> Database::quickCheck() {
    auto stmt = prepare("PRAGMA quick_check;");
    if (!stmt) {
        return std::move(stmt).error();
    }
    auto row = stmt->step();
    if (!row) {
        return std::move(row).error();
    }
    if (!row.value()) {
        return makeError(ErrorCode::Database, "The database integrity check returned no result.");
    }
    return stmt->columnText(0);
}

std::int64_t Database::lastInsertRowId() const { return sqlite3_last_insert_rowid(handle_); }

}  // namespace akeno::database
