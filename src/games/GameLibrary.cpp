// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/games/GameLibrary.hpp"

#include <algorithm>

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"

namespace akeno::games {

std::vector<GameInfo> applyFilter(std::vector<GameInfo> games, const LibraryFilter& filter) {
    games.erase(std::remove_if(games.begin(), games.end(),
                               [&](const GameInfo& game) {
                                   TitleKind kind = classifyTitleId(game.titleId);
                                   if (kind == TitleKind::Homebrew) return !filter.includeHomebrew;
                                   if (game.platform == Platform::Ps4 || kind == TitleKind::Ps4Game) {
                                       return !filter.includePs4;
                                   }
                                   return false;
                               }),
                games.end());

    auto byName = [](const GameInfo& a, const GameInfo& b) {
        std::string left = strings::toLowerAscii(a.name);
        std::string right = strings::toLowerAscii(b.name);
        if (left != right) return left < right;
        return a.titleId < b.titleId;
    };
    switch (filter.sort) {
        case database::LibrarySort::Name:
            std::stable_sort(games.begin(), games.end(), byName);
            break;
        case database::LibrarySort::TitleId:
            std::stable_sort(games.begin(), games.end(),
                             [](const GameInfo& a, const GameInfo& b) { return a.titleId < b.titleId; });
            break;
        case database::LibrarySort::RecentlyPlayed:
            std::stable_sort(games.begin(), games.end(), [&](const GameInfo& a, const GameInfo& b) {
                if (a.lastAccessTime.empty() != b.lastAccessTime.empty()) return !a.lastAccessTime.empty();
                if (a.lastAccessTime != b.lastAccessTime) return a.lastAccessTime > b.lastAccessTime;
                return byName(a, b);
            });
            break;
    }
    return games;
}

Result<LibrarySnapshot> GameLibrary::refresh() {
    auto discovered = provider_.discoverGames();
    if (!discovered) {
        return std::move(discovered).error();
    }
    LibrarySnapshot snapshot;
    snapshot.games = std::move(discovered).value();
    snapshot.refreshedAt = strings::utcTimestamp();
    if (db_ != nullptr) {
        auto recorded = recordHistory(snapshot.games, snapshot.versionChanges);
        if (!recorded) {
            // History is a convenience; discovery itself succeeded.
            logging::logger().warn("games", "Could not record game history: " + recorded.error().describe());
        }
    }
    for (const auto& change : snapshot.versionChanges) {
        logging::logger().info("games", "version changed: " + change);
    }
    logging::logger().info("games", strings::concat("library refreshed: ", snapshot.games.size(), " games"));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = snapshot;
    }
    return snapshot;
}

std::optional<LibrarySnapshot> GameLibrary::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::vector<GameInfo> GameLibrary::view(const LibraryFilter& filter) const {
    std::vector<GameInfo> games;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_) {
            return {};
        }
        games = snapshot_->games;
    }
    return applyFilter(std::move(games), filter);
}

std::optional<GameInfo> GameLibrary::find(std::string_view titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!snapshot_) {
        return std::nullopt;
    }
    for (const auto& game : snapshot_->games) {
        if (game.titleId == titleId) {
            return game;
        }
    }
    return std::nullopt;
}

Status GameLibrary::recordHistory(std::vector<GameInfo>& games, std::vector<std::string>& changes) {
    std::lock_guard<std::mutex> lock(db_->mutex());
    const std::string now = strings::utcTimestamp();
    return db_->transaction([&]() -> Status {
        auto select = db_->prepare("SELECT version, previous_version FROM games WHERE title_id = ?;");
        if (!select) return std::move(select).error();
        auto insert = db_->prepare(
            "INSERT INTO games(title_id, name, version, content_id, platform, source_type, install_path, "
            "runtime_path, first_seen_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
        if (!insert) return std::move(insert).error();
        auto update = db_->prepare(
            "UPDATE games SET name = ?, version = ?, content_id = ?, platform = ?, source_type = ?, "
            "install_path = ?, runtime_path = ?, last_seen_at = ?, "
            "previous_version = COALESCE(?, previous_version), "
            "version_changed_at = CASE WHEN ? IS NOT NULL THEN ? ELSE version_changed_at END "
            "WHERE title_id = ?;");
        if (!update) return std::move(update).error();

        for (GameInfo& game : games) {
            AKENO_TRY(select->bind(1, std::string_view(game.titleId)));
            auto row = select->step();
            if (!row) return std::move(row).error();
            const bool exists = row.value();
            std::string storedVersion = exists ? select->columnText(0) : std::string();
            std::optional<std::string> storedPrevious;
            if (exists && !select->columnIsNull(1)) storedPrevious = select->columnText(1);
            AKENO_TRY(select->reset());

            if (!exists) {
                AKENO_TRY(insert->bind(1, std::string_view(game.titleId)));
                AKENO_TRY(insert->bind(2, std::string_view(game.name)));
                AKENO_TRY(insert->bind(3, std::string_view(game.version)));
                AKENO_TRY(insert->bind(4, std::string_view(game.contentId)));
                AKENO_TRY(insert->bind(5, toString(game.platform)));
                AKENO_TRY(insert->bind(6, toString(game.sourceType)));
                AKENO_TRY(insert->bind(7, std::string_view(game.installPath)));
                AKENO_TRY(insert->bind(8, std::string_view(game.runtimePath)));
                AKENO_TRY(insert->bind(9, std::string_view(now)));
                AKENO_TRY(insert->bind(10, std::string_view(now)));
                auto done = insert->step();
                if (!done) return std::move(done).error();
                AKENO_TRY(insert->reset());
                continue;
            }

            const bool changed = !storedVersion.empty() && !game.version.empty() && storedVersion != game.version;
            if (changed) {
                game.previousVersion = storedVersion;
                changes.push_back(game.titleId + ": " + storedVersion + " -> " + game.version);
            } else if (storedPrevious) {
                game.previousVersion = storedPrevious;
            }
            AKENO_TRY(update->bind(1, std::string_view(game.name)));
            AKENO_TRY(update->bind(2, std::string_view(game.version)));
            AKENO_TRY(update->bind(3, std::string_view(game.contentId)));
            AKENO_TRY(update->bind(4, toString(game.platform)));
            AKENO_TRY(update->bind(5, toString(game.sourceType)));
            AKENO_TRY(update->bind(6, std::string_view(game.installPath)));
            AKENO_TRY(update->bind(7, std::string_view(game.runtimePath)));
            AKENO_TRY(update->bind(8, std::string_view(now)));
            if (changed) {
                AKENO_TRY(update->bind(9, std::string_view(storedVersion)));
                AKENO_TRY(update->bind(10, std::string_view(now)));
                AKENO_TRY(update->bind(11, std::string_view(now)));
            } else {
                AKENO_TRY(update->bindNull(9));
                AKENO_TRY(update->bindNull(10));
                AKENO_TRY(update->bindNull(11));
            }
            AKENO_TRY(update->bind(12, std::string_view(game.titleId)));
            auto done = update->step();
            if (!done) return std::move(done).error();
            AKENO_TRY(update->reset());
        }
        return {};
    });
}

}  // namespace akeno::games
