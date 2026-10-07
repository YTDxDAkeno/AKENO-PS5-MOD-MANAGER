// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/providers/AkenoCatalogProvider.hpp"

using namespace akeno;
using namespace akeno::providers;
using network::HttpMethod;

namespace {

const std::string kBase = "https://catalog.example/akeno/";

void serveFixtures(test::MockHttpClient& http) {
    for (const char* path : {"index.json", "games/example-blade.json", "games/neon-harbor.json",
                             "mods/example-blade/crimson-outfit.json", "mods/example-blade/sharper-foliage.json",
                             "mods/example-blade/photo-mode-ue4ss.json", "mods/example-blade/akeno-test-file.json"}) {
        http.respond(HttpMethod::Get, kBase + path, 200, test::readFixture(std::string("catalog/") + path));
    }
}

GameContext exampleBlade(std::string version = "01.011.000") { return GameContext{"PPSA90001", std::move(version)}; }

}  // namespace

TEST_CASE("catalogue addresses must be secure") {
    test::MockHttpClient http;
    CHECK_FALSE(AkenoCatalogProvider::create(http, "http://example.com/catalog/").ok());
    CHECK_FALSE(AkenoCatalogProvider::create(http, "https://example.com/catalog/?x=1").ok());
    auto provider = AkenoCatalogProvider::create(http, "https://example.com/catalog");
    REQUIRE(provider.ok());
    CHECK(provider.value()->baseUrl() == "https://example.com/catalog/");
}

TEST_CASE("browse a game's mods with version-aware labels") {
    test::MockHttpClient http;
    serveFixtures(http);
    auto provider = AkenoCatalogProvider::create(http, kBase).value();

    auto games = provider->listGames(nullptr);
    REQUIRE(games.ok());
    CHECK(games->size() == 3);

    SearchQuery query;
    query.game = exampleBlade();
    auto page = provider->browseMods(query, nullptr);
    REQUIRE(page.ok());
    CHECK(page->total == 6);
    REQUIRE(page->mods.size() == 6);
    CHECK(page->mods[0].name == "Crimson Outfit Recolour");
    CHECK(page->mods[0].compatibility == CompatibilityStatus::Verified);
    CHECK(page->mods[0].ref.modId == "example-blade/crimson-outfit");
    CHECK(page->mods[1].compatibility == CompatibilityStatus::Experimental);  // verified for another version
    CHECK(page->mods[3].compatibility == CompatibilityStatus::PcOnly);

    query.order = BrowseOrder::Verified;
    auto sorted = provider->browseMods(query, nullptr);
    REQUIRE(sorted.ok());
    CHECK(sorted->mods.front().compatibility == CompatibilityStatus::Verified);
    CHECK(sorted->mods.back().compatibility == CompatibilityStatus::PcOnly);

    query.order = BrowseOrder::Newest;
    auto newest = provider->browseMods(query, nullptr);
    REQUIRE(newest.ok());
    CHECK(newest->mods.front().name == "Akeno Harmless Test Package");

    query.offset = 4;
    query.limit = 10;
    query.order = BrowseOrder::Featured;
    auto paged = provider->browseMods(query, nullptr);
    REQUIRE(paged.ok());
    CHECK(paged->total == 6);
    CHECK(paged->mods.size() == 2);
}

TEST_CASE("search matches name, author, summary and category") {
    test::MockHttpClient http;
    serveFixtures(http);
    auto provider = AkenoCatalogProvider::create(http, kBase).value();
    SearchQuery query;
    query.game = exampleBlade();
    query.text = "HAIR";
    auto hair = provider->searchMods(query, nullptr);
    REQUIRE(hair.ok());
    REQUIRE(hair->mods.size() == 1);
    CHECK(hair->mods[0].name == "Alternative Hair Colours");
    query.text = "textures";
    CHECK(provider->searchMods(query, nullptr)->mods.size() == 1);
    query.text = "nothing-matches-this";
    CHECK(provider->searchMods(query, nullptr)->mods.empty());
}

TEST_CASE("a game without catalogue entries is reported clearly") {
    test::MockHttpClient http;
    serveFixtures(http);
    auto provider = AkenoCatalogProvider::create(http, kBase).value();
    SearchQuery query;
    query.game = GameContext{"PPSA00042", "1.0"};
    auto page = provider->browseMods(query, nullptr);
    REQUIRE_FALSE(page.ok());
    CHECK(page.error().code == ErrorCode::NotFound);
    CHECK(page.error().message == "The Akeno Catalogue has no mods for this game yet.");
}

TEST_CASE("details include compatibility checks against the installed game") {
    test::MockHttpClient http;
    serveFixtures(http);
    auto provider = AkenoCatalogProvider::create(http, kBase).value();
    ModRef ref{"akeno-catalogue", "example-blade/crimson-outfit"};
    auto details = provider->getModDetails(ref, exampleBlade(), nullptr);
    REQUIRE(details.ok());
    CHECK(details->summary.compatibility == CompatibilityStatus::Verified);
    CHECK(details->risk == "LOW");
    CHECK(details->installable);
    CHECK(details->screenshots.size() == 2);
    REQUIRE(details->files.size() == 1);
    CHECK(details->files[0].sha256.size() == 64);

    auto mismatch = provider->getModDetails(ref, exampleBlade("01.020.000"), nullptr);
    REQUIRE(mismatch.ok());
    CHECK(mismatch->summary.compatibility == CompatibilityStatus::Experimental);
    CHECK(mismatch->needsConfirmation);

    auto pc = provider->getModDetails({"akeno-catalogue", "example-blade/photo-mode-ue4ss"}, exampleBlade(), nullptr);
    REQUIRE(pc.ok());
    CHECK(pc->summary.compatibility == CompatibilityStatus::PcOnly);
    CHECK_FALSE(pc->installable);

    auto ticket = provider->resolveDownload(ref, details->files[0], nullptr);
    REQUIRE(ticket.ok());
    CHECK(ticket->expectedSha256 == details->files[0].sha256);
    CHECK(provider->getLatestVersion(ref, nullptr).value() == "1.2.0");
}

TEST_CASE("mod references are validated before any request") {
    test::MockHttpClient http;
    auto provider = AkenoCatalogProvider::create(http, kBase).value();
    for (const char* bad : {"../x/y", "example-blade/../../etc", "example-blade", "Example/x", "a/b/c"}) {
        CAPTURE(bad);
        CHECK_FALSE(provider->getModDetails({"akeno-catalogue", bad}, std::nullopt, nullptr).ok());
    }
    CHECK_FALSE(provider->getModDetails({"nexus", "example-blade/x"}, std::nullopt, nullptr).ok());
    CHECK(http.requests().empty());
}

TEST_CASE("documents are cached and the cache can be cleared") {
    test::MockHttpClient http;
    serveFixtures(http);
    auto provider = AkenoCatalogProvider::create(http, kBase).value();
    REQUIRE(provider->listGames(nullptr).ok());
    REQUIRE(provider->listGames(nullptr).ok());
    CHECK(http.requests().size() == 1);
    provider->clearCache();
    REQUIRE(provider->listGames(nullptr).ok());
    CHECK(http.requests().size() == 2);
}

TEST_CASE("server errors are explained") {
    test::MockHttpClient http;
    http.respond(HttpMethod::Get, kBase + "index.json", 500, "oops");
    auto provider = AkenoCatalogProvider::create(http, kBase).value();
    auto games = provider->listGames(nullptr);
    REQUIRE_FALSE(games.ok());
    CHECK(games.error().code == ErrorCode::HttpStatus);
}
