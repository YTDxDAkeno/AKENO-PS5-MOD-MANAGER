// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/database/SettingsStore.hpp"

#include <charconv>
#include <map>
#include <mutex>

#include "akeno/core/Strings.hpp"
#include "akeno/network/Url.hpp"

namespace akeno::database {

namespace {

constexpr std::string_view kFirstRun = "app.first_run_complete";
constexpr std::string_view kShowPs4 = "library.show_ps4";
constexpr std::string_view kShowHomebrew = "library.show_homebrew";
constexpr std::string_view kSort = "library.sort";
constexpr std::string_view kSmPort = "shadowmount.port";
constexpr std::string_view kDebugLogging = "logging.debug";
constexpr std::string_view kProbeUrl = "network.probe_url";
constexpr std::string_view kCatalogueUrl = "catalogue.url";

bool parseBool(const std::string& text, bool& out) {
    if (text == "1" || text == "true") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parseSort(const std::string& text, LibrarySort& out) {
    if (text == "name") out = LibrarySort::Name;
    else if (text == "title_id") out = LibrarySort::TitleId;
    else if (text == "recent") out = LibrarySort::RecentlyPlayed;
    else return false;
    return true;
}

}  // namespace

bool isValidCatalogueUrl(std::string_view url) {
    auto parsed = network::parseUrl(url);
    return parsed && (parsed->isHttps() || network::isLoopbackHost(parsed->host)) &&
           parsed->target.find('?') == std::string::npos;
}

std::string_view toString(LibrarySort sort) noexcept {
    switch (sort) {
        case LibrarySort::Name: return "name";
        case LibrarySort::TitleId: return "title_id";
        case LibrarySort::RecentlyPlayed: return "recent";
    }
    return "name";
}

Result<SettingsLoadResult> SettingsStore::load() {
    std::lock_guard<std::mutex> lock(db_.mutex());
    auto stmt = db_.prepare("SELECT key, value FROM settings;");
    if (!stmt) {
        return std::move(stmt).error();
    }
    std::map<std::string, std::string> stored;
    while (true) {
        auto row = stmt->step();
        if (!row) {
            return std::move(row).error();
        }
        if (!row.value()) break;
        stored[stmt->columnText(0)] = stmt->columnText(1);
    }

    SettingsLoadResult result;
    Settings& s = result.settings;
    auto reject = [&](std::string_view key) {
        result.warnings.push_back(strings::concat("Invalid stored value for ", key, "; using the default."));
    };
    auto readBool = [&](std::string_view key, bool& target) {
        auto it = stored.find(std::string(key));
        if (it != stored.end() && !parseBool(it->second, target)) reject(key);
    };
    readBool(kFirstRun, s.firstRunComplete);
    readBool(kShowPs4, s.showPs4Games);
    readBool(kShowHomebrew, s.showHomebrew);
    readBool(kDebugLogging, s.debugLogging);
    if (auto it = stored.find(std::string(kSort)); it != stored.end() && !parseSort(it->second, s.librarySort)) {
        reject(kSort);
    }
    if (auto it = stored.find(std::string(kSmPort)); it != stored.end()) {
        int port = 0;
        const std::string& text = it->second;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
        if (ec == std::errc() && ptr == text.data() + text.size() && port >= 1 && port <= 65535) {
            s.shadowMountPort = port;
        } else {
            reject(kSmPort);
        }
    }
    if (auto it = stored.find(std::string(kProbeUrl)); it != stored.end()) {
        auto url = network::parseUrl(it->second);
        if (url && url->isHttps()) {
            s.networkProbeUrl = it->second;
        } else {
            reject(kProbeUrl);
        }
    }
    if (auto it = stored.find(std::string(kCatalogueUrl)); it != stored.end()) {
        if (it->second == kLegacyDefaultCatalogueUrl) {
            s.catalogueUrl = std::string(kDefaultCatalogueUrl);  // the old default follows the new one
        } else if (isValidCatalogueUrl(it->second)) {
            s.catalogueUrl = it->second;
        } else {
            reject(kCatalogueUrl);
        }
    }
    return result;
}

Status SettingsStore::save(const Settings& settings) {
    std::lock_guard<std::mutex> lock(db_.mutex());
    const std::string now = strings::utcTimestamp();
    const std::vector<std::pair<std::string_view, std::string>> values{
        {kFirstRun, settings.firstRunComplete ? "1" : "0"},
        {kShowPs4, settings.showPs4Games ? "1" : "0"},
        {kShowHomebrew, settings.showHomebrew ? "1" : "0"},
        {kSort, std::string(toString(settings.librarySort))},
        {kSmPort, std::to_string(settings.shadowMountPort)},
        {kDebugLogging, settings.debugLogging ? "1" : "0"},
        {kProbeUrl, settings.networkProbeUrl},
        {kCatalogueUrl, settings.catalogueUrl},
    };
    return db_.transaction([&]() -> Status {
        auto stmt = db_.prepare(
            "INSERT INTO settings(key, value, updated_at) VALUES (?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;");
        if (!stmt) {
            return std::move(stmt).error();
        }
        for (const auto& [key, value] : values) {
            AKENO_TRY(stmt->bind(1, key));
            AKENO_TRY(stmt->bind(2, std::string_view(value)));
            AKENO_TRY(stmt->bind(3, std::string_view(now)));
            auto done = stmt->step();
            if (!done) {
                return std::move(done).error();
            }
            AKENO_TRY(stmt->reset());
        }
        return {};
    });
}

}  // namespace akeno::database
