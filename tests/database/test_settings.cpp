// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/database/Migrations.hpp"
#include "akeno/database/SettingsStore.hpp"

using namespace akeno;
using namespace akeno::database;

namespace {

std::unique_ptr<Database> freshDatabase() {
    auto db = Database::openInMemory();
    REQUIRE(db.ok());
    REQUIRE(migrate(*db.value(), builtinMigrations(), {}).ok());
    return std::move(db).value();
}

}  // namespace

TEST_CASE("defaults are returned for an empty settings table") {
    auto db = freshDatabase();
    SettingsStore store(*db);
    auto loaded = store.load();
    REQUIRE(loaded.ok());
    CHECK(loaded->warnings.empty());
    CHECK_FALSE(loaded->settings.firstRunComplete);
    CHECK(loaded->settings.shadowMountPort == 10101);
    CHECK(loaded->settings.librarySort == LibrarySort::Name);
}

TEST_CASE("settings round-trip") {
    auto db = freshDatabase();
    SettingsStore store(*db);
    Settings settings;
    settings.firstRunComplete = true;
    settings.showPs4Games = false;
    settings.showHomebrew = true;
    settings.librarySort = LibrarySort::RecentlyPlayed;
    settings.shadowMountPort = 12345;
    settings.debugLogging = true;
    settings.networkProbeUrl = "https://example.org/";
    settings.catalogueUrl = "https://mods.example.org/akeno/";
    REQUIRE(store.save(settings).ok());
    REQUIRE(store.save(settings).ok());  // upsert, not duplicate
    auto loaded = store.load();
    REQUIRE(loaded.ok());
    const Settings& got = loaded->settings;
    CHECK(got.firstRunComplete);
    CHECK_FALSE(got.showPs4Games);
    CHECK(got.showHomebrew);
    CHECK(got.librarySort == LibrarySort::RecentlyPlayed);
    CHECK(got.shadowMountPort == 12345);
    CHECK(got.debugLogging);
    CHECK(got.networkProbeUrl == "https://example.org/");
    CHECK(got.catalogueUrl == "https://mods.example.org/akeno/");
}

TEST_CASE("invalid stored values fall back to defaults with a warning") {
    auto db = freshDatabase();
    REQUIRE(db->exec("INSERT INTO settings VALUES ('shadowmount.port', '99999', 'x');").ok());
    REQUIRE(db->exec("INSERT INTO settings VALUES ('library.sort', 'random', 'x');").ok());
    REQUIRE(db->exec("INSERT INTO settings VALUES ('library.show_ps4', 'maybe', 'x');").ok());
    REQUIRE(db->exec("INSERT INTO settings VALUES ('network.probe_url', 'http://insecure.example/', 'x');").ok());
    REQUIRE(db->exec("INSERT INTO settings VALUES ('catalogue.url', 'http://insecure.example/catalog/', 'x');").ok());
    SettingsStore store(*db);
    auto loaded = store.load();
    REQUIRE(loaded.ok());
    CHECK(loaded->warnings.size() == 5);
    CHECK(loaded->settings.catalogueUrl == kDefaultCatalogueUrl);
    CHECK(loaded->settings.shadowMountPort == 10101);
    CHECK(loaded->settings.librarySort == LibrarySort::Name);
    CHECK(loaded->settings.showPs4Games);
    CHECK(loaded->settings.networkProbeUrl == "https://raw.githubusercontent.com/");
}

TEST_CASE("catalogue addresses: https anywhere, plain http only on this machine") {
    CHECK(isValidCatalogueUrl(kDefaultCatalogueUrl));
    CHECK(isValidCatalogueUrl("https://mods.example.org/akeno/"));
    CHECK(isValidCatalogueUrl("http://127.0.0.1:10101/catalog/"));
    CHECK(isValidCatalogueUrl("http://localhost/catalog/"));
    CHECK_FALSE(isValidCatalogueUrl("http://192.168.1.20/catalog/"));
    CHECK_FALSE(isValidCatalogueUrl("http://mods.example.org/"));
    CHECK_FALSE(isValidCatalogueUrl("https://mods.example.org/?list=1"));
    CHECK_FALSE(isValidCatalogueUrl("https://user:pass@mods.example.org/"));
    CHECK_FALSE(isValidCatalogueUrl("file:///data/catalog/"));
    CHECK_FALSE(isValidCatalogueUrl(""));
}

TEST_CASE("the old main-branch catalogue default follows the new default") {
    auto db = freshDatabase();
    REQUIRE(db->exec(std::string("INSERT INTO settings VALUES ('catalogue.url', '") +
                     std::string(kLegacyDefaultCatalogueUrl) + "', 'x');")
                .ok());
    SettingsStore store(*db);
    auto loaded = store.load();
    REQUIRE(loaded.ok());
    CHECK(loaded->warnings.empty());
    CHECK(loaded->settings.catalogueUrl == kDefaultCatalogueUrl);
}
