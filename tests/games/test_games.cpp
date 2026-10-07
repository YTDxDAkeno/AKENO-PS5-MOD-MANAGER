// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/database/Migrations.hpp"
#include "akeno/games/GameLibrary.hpp"

using namespace akeno;
using namespace akeno::games;

TEST_CASE("title ID validation") {
    CHECK(isValidTitleId("PPSA01234"));
    CHECK(isValidTitleId("CUSA00001"));
    CHECK_FALSE(isValidTitleId("ppsa01234"));
    CHECK_FALSE(isValidTitleId("PPSA0123"));
    CHECK_FALSE(isValidTitleId("PPSA012345"));
    CHECK_FALSE(isValidTitleId("PPSA0123A"));
    CHECK_FALSE(isValidTitleId("../../etc"));
    CHECK_FALSE(isValidTitleId(""));
    CHECK(classifyTitleId("PPSA01234") == TitleKind::Ps5Game);
    CHECK(classifyTitleId("CUSA01234") == TitleKind::Ps4Game);
    CHECK(classifyTitleId("LAPY01234") == TitleKind::Homebrew);
    CHECK(classifyTitleId("FAKE01234") == TitleKind::Homebrew);
    CHECK(classifyTitleId("NPXS40028") == TitleKind::Other);
}

TEST_CASE("library filter hides PS4 games and homebrew on request") {
    std::vector<GameInfo> games{
        test::makeGame("PPSA00002", "zeta", "1.0"),
        test::makeGame("CUSA00001", "Alpha PS4", "1.0", Platform::Ps4),
        test::makeGame("LAPY00001", "Homebrew", "1.0"),
        test::makeGame("PPSA00001", "Beta", "1.0"),
    };
    LibraryFilter filter;
    filter.includePs4 = false;
    filter.includeHomebrew = false;
    auto view = applyFilter(games, filter);
    REQUIRE(view.size() == 2);
    CHECK(view[0].name == "Beta");  // case-insensitive name order
    CHECK(view[1].name == "zeta");

    filter.includePs4 = true;
    filter.includeHomebrew = true;
    CHECK(applyFilter(games, filter).size() == 4);
}

TEST_CASE("library sort orders") {
    auto a = test::makeGame("PPSA00003", "Alpha", "1");
    a.lastAccessTime = "2026-01-01 00:00:00";
    auto b = test::makeGame("PPSA00001", "Bravo", "1");
    b.lastAccessTime = "2026-05-01 00:00:00";
    auto c = test::makeGame("PPSA00002", "Charlie", "1");
    LibraryFilter filter;
    filter.sort = database::LibrarySort::TitleId;
    auto byId = applyFilter({a, b, c}, filter);
    CHECK(byId[0].titleId == "PPSA00001");
    filter.sort = database::LibrarySort::RecentlyPlayed;
    auto recent = applyFilter({a, b, c}, filter);
    CHECK(recent[0].name == "Bravo");
    CHECK(recent[1].name == "Alpha");
    CHECK(recent[2].name == "Charlie");  // never played sorts last
}

TEST_CASE("GameLibrary records versions and reports changes") {
    auto dbResult = database::Database::openInMemory();
    REQUIRE(dbResult.ok());
    auto db = std::move(dbResult).value();
    REQUIRE(database::migrate(*db, database::builtinMigrations(), {}).ok());

    test::FakeGameProvider provider;
    provider.games = std::vector<GameInfo>{test::makeGame("PPSA01234", "Example Blade", "1.010")};
    GameLibrary library(provider, db.get());

    auto first = library.refresh();
    REQUIRE(first.ok());
    CHECK(first->versionChanges.empty());
    CHECK_FALSE(first->games[0].previousVersion.has_value());

    provider.games = std::vector<GameInfo>{test::makeGame("PPSA01234", "Example Blade", "1.020")};
    auto second = library.refresh();
    REQUIRE(second.ok());
    REQUIRE(second->versionChanges.size() == 1);
    CHECK(second->versionChanges[0] == "PPSA01234: 1.010 -> 1.020");
    CHECK(second->games[0].previousVersion == std::optional<std::string>("1.010"));

    // The previous version stays visible on later refreshes.
    auto third = library.refresh();
    REQUIRE(third.ok());
    CHECK(third->versionChanges.empty());
    CHECK(third->games[0].previousVersion == std::optional<std::string>("1.010"));

    auto found = library.find("PPSA01234");
    REQUIRE(found.has_value());
    CHECK(found->version == "1.020");
    CHECK_FALSE(library.find("PPSA99999").has_value());
}

TEST_CASE("GameLibrary works without a database") {
    test::FakeGameProvider provider;
    provider.games = std::vector<GameInfo>{test::makeGame("PPSA01234", "Example", "1.0")};
    GameLibrary library(provider, nullptr);
    auto snapshot = library.refresh();
    REQUIRE(snapshot.ok());
    CHECK(snapshot->games.size() == 1);
    CHECK(library.view({}).size() == 1);
}

TEST_CASE("GameLibrary propagates discovery errors and keeps the previous snapshot") {
    test::FakeGameProvider provider;
    provider.games = std::vector<GameInfo>{test::makeGame("PPSA01234", "Example", "1.0")};
    GameLibrary library(provider, nullptr);
    REQUIRE(library.refresh().ok());
    provider.games = makeError(ErrorCode::Unavailable, "ShadowMountPlus is not answering");
    auto failed = library.refresh();
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == ErrorCode::Unavailable);
    CHECK(library.view({}).size() == 1);
}
