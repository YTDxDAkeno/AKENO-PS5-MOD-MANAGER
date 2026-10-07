// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/mods/Catalog.hpp"

using namespace akeno;
using namespace akeno::mods;

namespace {

std::string manifestFixture() { return test::readFixture("catalog/mods/example-blade/crimson-outfit.json"); }

// Parses the fixture, applies `edit` to the JSON and returns the parse result of the edited text.
Result<ModManifest> parseEdited(const std::function<void(json::Json&)>& edit) {
    json::Json doc = json::Json::parse(manifestFixture());
    edit(doc);
    return parseModManifest(doc.dump(), "crimson-outfit");
}

}  // namespace

TEST_CASE("catalogue identifiers and paths") {
    CHECK(isValidCatalogId("example-blade"));
    CHECK(isValidCatalogId("a"));
    CHECK_FALSE(isValidCatalogId(""));
    CHECK_FALSE(isValidCatalogId("-leading"));
    CHECK_FALSE(isValidCatalogId("Upper"));
    CHECK_FALSE(isValidCatalogId("../x"));
    CHECK_FALSE(isValidCatalogId("a/b"));
    CHECK_FALSE(isValidCatalogId(std::string(64, 'a')));

    CHECK(isSafeRelativePath(""));
    CHECK(isSafeRelativePath("Content/Paks/~mods"));
    CHECK_FALSE(isSafeRelativePath("/abs"));
    CHECK_FALSE(isSafeRelativePath("a/../b"));
    CHECK_FALSE(isSafeRelativePath("a//b"));
    CHECK_FALSE(isSafeRelativePath("a\\b"));
    CHECK_FALSE(isSafeRelativePath("./a"));
    CHECK_FALSE(isSafeRelativePath("C:/x"));

    CHECK(isAllowedRemoteUrl("https://raw.githubusercontent.com/a/b/main/catalog/"));
    CHECK(isAllowedRemoteUrl("http://127.0.0.1:10101/catalog/"));
    CHECK_FALSE(isAllowedRemoteUrl("http://example.com/catalog/"));
    CHECK_FALSE(isAllowedRemoteUrl("ftp://example.com/"));
}

TEST_CASE("the demo catalogue index and game files parse") {
    auto index = parseCatalogIndex(test::readFixture("catalog/index.json"));
    REQUIRE(index.ok());
    CHECK(index->games.size() == 3);
    REQUIRE(index->findByTitleId("PPSA90001") != nullptr);
    CHECK(index->findByTitleId("PPSA90001")->id == "example-blade");
    CHECK(index->findByTitleId("PPSA00000") == nullptr);

    auto game = parseCatalogGame(test::readFixture("catalog/games/example-blade.json"), "example-blade");
    REQUIRE(game.ok());
    CHECK(game->mods.size() == 6);
    CHECK(game->mods[0].claimed == ClaimedStatus::Verified);
    CHECK(game->mods[0].gameVersions == std::vector<std::string>{"01.011.000"});
    CHECK(game->mods[3].claimed == ClaimedStatus::PcOnly);

    CHECK_FALSE(parseCatalogGame(test::readFixture("catalog/games/example-blade.json"), "neon-harbor").ok());
}

TEST_CASE("a full manifest parses with every section") {
    auto m = parseModManifest(manifestFixture(), "crimson-outfit");
    REQUIRE(m.ok());
    CHECK(m->name == "Crimson Outfit Recolour");
    CHECK(m->version == "1.2.0");
    CHECK(m->titleIds == std::vector<std::string>{"PPSA90001"});
    CHECK(m->gameVersions == std::vector<std::string>{"01.011.000"});
    CHECK(m->platform == "ps5");
    CHECK(m->claimed == ClaimedStatus::Verified);
    CHECK(m->modType == ModType::AssetReplacement);
    CHECK(m->format == ArchiveFormat::Zip);
    CHECK(m->installMethod == "shadowmount-overlay");
    CHECK(m->downloadSha256.size() == 64);
    CHECK(m->screenshots.size() == 2);
}

TEST_CASE("manifest validation rejects unsafe or incomplete entries") {
    CHECK_FALSE(parseEdited([](json::Json& d) { d["download"]["url"] = "http://evil.example/x.zip"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["download"]["url"] = "file:///etc/passwd"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["download"]["sha256"] = "ABC"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["download"].erase("sha256"); }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["download"]["size"] = 0; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["download"]["size"] = 1000000000000000LL; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["game"]["titleIds"] = json::Json::array(); }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["game"]["titleIds"] = {"../../etc"}; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["installation"]["archiveRoot"] = "../outside"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["installation"]["targetPrefix"] = "/system"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["id"] = "other-id"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["version"] = "1.0 beta"; }).ok());
    CHECK_FALSE(parseEdited([](json::Json& d) { d["dependencies"]["requires"] = {"Not Valid"}; }).ok());

    auto future = parseEdited([](json::Json& d) { d["schemaVersion"] = 2; });
    REQUIRE_FALSE(future.ok());
    CHECK(future.error().code == ErrorCode::Unsupported);
}

TEST_CASE("unknown values degrade safely") {
    auto m = parseEdited([](json::Json& d) {
        d["compatibility"]["status"] = "definitely-works";
        d["compatibility"]["modType"] = "dll-injection";
        d["media"]["screenshots"] = {{{"url", "http://evil.example/a.png"}}, {{"url", "https://ok.example/b.png"}}};
        d["unexpectedField"] = 42;
    });
    REQUIRE(m.ok());
    CHECK(m->claimed == ClaimedStatus::Unknown);
    CHECK(m->modType == ModType::Other);
    REQUIRE(m->screenshots.size() == 1);  // the plain-http remote image is dropped
    CHECK(m->screenshots[0].url == "https://ok.example/b.png");
}

TEST_CASE("oversized or malformed documents are rejected") {
    CHECK_FALSE(parseCatalogIndex(std::string(2 * 1024 * 1024, ' ')).ok());
    CHECK_FALSE(parseCatalogIndex("[]").ok());
    CHECK_FALSE(parseCatalogIndex(R"({"schemaVersion":1})").ok());
    CHECK_FALSE(parseCatalogIndex(R"({"schemaVersion":1,"games":[{"id":"a","titleIds":[]}]})").ok());
    CHECK_FALSE(parseCatalogIndex(R"({"schemaVersion":1,"games":[{"id":"a","titleIds":["PPSA00001"]},{"id":"a","titleIds":["PPSA00002"]}]})").ok());
}
