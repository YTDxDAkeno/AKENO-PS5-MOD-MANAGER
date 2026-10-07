// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/database/SettingsStore.hpp"
#include "akeno/games/IGameDiscoveryProvider.hpp"

namespace akeno::games {

struct LibraryFilter {
    bool includePs4 = true;
    bool includeHomebrew = false;
    database::LibrarySort sort = database::LibrarySort::Name;
};

// Pure function used by the library view; exposed for tests.
std::vector<GameInfo> applyFilter(std::vector<GameInfo> games, const LibraryFilter& filter);

struct LibrarySnapshot {
    std::vector<GameInfo> games;   // unfiltered, as discovered
    std::string refreshedAt;
    std::vector<std::string> versionChanges;  // "PPSA01234: 1.010 -> 1.020"
};

class GameLibrary {
public:
    // `db` may be null (database unavailable); the library then works without history.
    GameLibrary(IGameDiscoveryProvider& provider, database::Database* db) : provider_(provider), db_(db) {}

    // Runs discovery (blocking; call from a worker) and stores the snapshot.
    Result<LibrarySnapshot> refresh();

    std::optional<LibrarySnapshot> snapshot() const;
    std::vector<GameInfo> view(const LibraryFilter& filter) const;
    std::optional<GameInfo> find(std::string_view titleId) const;

    IGameDiscoveryProvider& provider() noexcept { return provider_; }

private:
    Status recordHistory(std::vector<GameInfo>& games, std::vector<std::string>& changes);

    IGameDiscoveryProvider& provider_;
    database::Database* db_;
    mutable std::mutex mutex_;
    std::optional<LibrarySnapshot> snapshot_;
};

}  // namespace akeno::games
