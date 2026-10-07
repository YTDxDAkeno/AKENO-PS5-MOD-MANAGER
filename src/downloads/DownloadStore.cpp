// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/downloads/DownloadStore.hpp"

#include <mutex>

#include "akeno/core/Strings.hpp"

namespace akeno::downloads {

Result<LoadedDownloads> DownloadStore::loadAll() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    auto stmt = db_.prepare(
        "SELECT id, provider_id, mod_id, name, mod_version, game_title_id, game_version, compatibility, url, "
        "expected_size, expected_sha256, format, state, bytes_done, attempts, error, created_at, updated_at, "
        "completed_at FROM downloads ORDER BY created_at, rowid;");
    if (!stmt) {
        return std::move(stmt).error();
    }
    LoadedDownloads loaded;
    while (true) {
        auto row = stmt->step();
        if (!row) {
            return std::move(row).error();
        }
        if (!row.value()) break;
        DownloadRecord record;
        record.id = stmt->columnText(0);
        DownloadRequest& request = record.request;
        request.mod.providerId = stmt->columnText(1);
        request.mod.modId = stmt->columnText(2);
        request.displayName = stmt->columnText(3);
        request.modVersion = stmt->columnText(4);
        request.gameTitleId = stmt->columnText(5);
        request.gameVersion = stmt->columnText(6);
        request.compatibility = stmt->columnText(7);
        request.url = stmt->columnText(8);
        const std::int64_t size = stmt->columnInt64(9);
        request.expectedSize = size > 0 ? static_cast<std::uint64_t>(size) : 0;
        request.expectedSha256 = stmt->columnText(10);
        request.format = mods::parseArchiveFormat(stmt->columnText(11));
        auto state = parseDownloadState(stmt->columnText(12));
        const std::int64_t done = stmt->columnInt64(13);
        record.bytesDone = done > 0 ? static_cast<std::uint64_t>(done) : 0;
        record.attempts = static_cast<int>(stmt->columnInt64(14));
        record.error = stmt->columnText(15);
        record.createdAt = stmt->columnText(16);
        record.updatedAt = stmt->columnText(17);
        record.completedAt = stmt->columnText(18);

        auto valid = validateRequest(request);
        if (!isValidDownloadId(record.id) || !state || !valid) {
            loaded.warnings.push_back(strings::concat("ignoring an invalid download record '",
                                                      strings::sanitizeForDisplay(record.id, 64), "': ",
                                                      valid ? std::string("bad id or state") : valid.error().describe()));
            continue;
        }
        record.state = *state;
        if (record.bytesDone > request.expectedSize) record.bytesDone = 0;
        loaded.records.push_back(std::move(record));
    }
    return loaded;
}

Status DownloadStore::save(const DownloadRecord& record) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    auto stmt = db_.prepare(
        "INSERT OR REPLACE INTO downloads(id, provider_id, mod_id, name, mod_version, game_title_id, game_version, "
        "compatibility, url, expected_size, expected_sha256, format, state, bytes_done, attempts, error, created_at, "
        "updated_at, completed_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!stmt) {
        return std::move(stmt).error();
    }
    const DownloadRequest& request = record.request;
    int i = 1;
    AKENO_TRY(stmt->bind(i++, std::string_view(record.id)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.mod.providerId)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.mod.modId)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.displayName)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.modVersion)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.gameTitleId)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.gameVersion)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.compatibility)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.url)));
    AKENO_TRY(stmt->bind(i++, static_cast<std::int64_t>(request.expectedSize)));
    AKENO_TRY(stmt->bind(i++, std::string_view(request.expectedSha256)));
    AKENO_TRY(stmt->bind(i++, mods::toString(request.format)));
    AKENO_TRY(stmt->bind(i++, toString(record.state)));
    AKENO_TRY(stmt->bind(i++, static_cast<std::int64_t>(record.bytesDone)));
    AKENO_TRY(stmt->bind(i++, static_cast<std::int64_t>(record.attempts)));
    AKENO_TRY(stmt->bind(i++, std::string_view(record.error)));
    AKENO_TRY(stmt->bind(i++, std::string_view(record.createdAt)));
    AKENO_TRY(stmt->bind(i++, std::string_view(record.updatedAt)));
    AKENO_TRY(stmt->bind(i++, std::string_view(record.completedAt)));
    auto done = stmt->step();
    if (!done) {
        return std::move(done).error();
    }
    return {};
}

Status DownloadStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    auto stmt = db_.prepare("DELETE FROM downloads WHERE id = ?;");
    if (!stmt) {
        return std::move(stmt).error();
    }
    AKENO_TRY(stmt->bind(1, std::string_view(id)));
    auto done = stmt->step();
    if (!done) {
        return std::move(done).error();
    }
    return {};
}

}  // namespace akeno::downloads
