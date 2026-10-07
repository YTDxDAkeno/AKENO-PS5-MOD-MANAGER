// SPDX-License-Identifier: GPL-3.0-or-later
// Typed application settings persisted in the `settings` table. Every value is validated on
// load; invalid stored values fall back to the default and are reported.
#pragma once

#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/database/Database.hpp"

namespace akeno::database {

enum class LibrarySort { Name, TitleId, RecentlyPlayed };

inline constexpr std::string_view kDefaultCatalogueUrl =
    "https://raw.githubusercontent.com/YTDxDAkeno/AKENO-PS5-MOD-MANAGER/main/catalog/";

// https:// anywhere, or http:// to a loopback address (local development).
bool isValidCatalogueUrl(std::string_view url);

struct Settings {
    bool firstRunComplete = false;
    bool showPs4Games = true;
    bool showHomebrew = false;          // LAPY/FAKE titles
    LibrarySort librarySort = LibrarySort::Name;
    int shadowMountPort = 10101;        // the host is always 127.0.0.1
    bool debugLogging = false;
    std::string networkProbeUrl = "https://raw.githubusercontent.com/";
    // Akeno Catalogue location: https, or http to this console for local testing.
    std::string catalogueUrl = std::string(kDefaultCatalogueUrl);
};

std::string_view toString(LibrarySort sort) noexcept;

struct SettingsLoadResult {
    Settings settings;
    std::vector<std::string> warnings;  // e.g. "invalid value for shadowmount.port, using default"
};

class SettingsStore {
public:
    explicit SettingsStore(Database& db) : db_(db) {}

    Result<SettingsLoadResult> load();
    Status save(const Settings& settings);

private:
    Database& db_;
};

}  // namespace akeno::database
