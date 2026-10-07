// SPDX-License-Identifier: GPL-3.0-or-later
// Storage layout of the application directory (see docs/architecture.md).
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace akeno {

struct AppPaths {
    std::filesystem::path root;

    std::filesystem::path database() const { return root / "database"; }
    std::filesystem::path databaseFile() const { return database() / "akeno.sqlite"; }
    std::filesystem::path downloads() const { return root / "downloads"; }
    std::filesystem::path cache() const { return root / "cache"; }
    std::filesystem::path iconCache() const { return cache() / "icons"; }
    std::filesystem::path imageCache() const { return cache() / "images"; }
    std::filesystem::path staging() const { return root / "staging"; }
    std::filesystem::path mods() const { return root / "mods"; }
    std::filesystem::path overlays() const { return root / "overlays"; }
    std::filesystem::path profiles() const { return root / "profiles"; }
    std::filesystem::path logs() const { return root / "logs"; }
    std::filesystem::path backups() const { return root / "backups"; }
    std::filesystem::path operationJournal() const { return root / "operation_state.json"; }

    // Directories created at startup, in creation order.
    std::vector<std::filesystem::path> layout() const {
        return {root,        database(), downloads(), cache(),     iconCache(), imageCache(),
                staging(),   mods(),     overlays(),  profiles(),  logs(),      backups()};
    }
};

// Default application directory on the console.
inline constexpr std::string_view kPs5DefaultDataRoot = "/data/akeno-mod-manager";

}  // namespace akeno
