// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/downloads/DownloadTypes.hpp"
#include "akeno/network/Url.hpp"
#include "akeno/providers/GameBananaProvider.hpp"

using namespace akeno;
using namespace akeno::providers;
using network::HttpMethod;

namespace {

const std::string kBase = "https://gamebanana.com";

struct Fixture {
    test::MockHttpClient http;
    GameBananaProvider banana{http, kBase};

    Fixture() {
        http.respond(HttpMethod::Get,
                     kBase + "/apiv11/Util/Game/NameMatch?_sName=" + network::percentEncode("Digimon Story Time Stranger"),
                     200,
                     R"({"_aRecords":[{"_idRow":21999,"_sName":"Digimon Story: Time Stranger","_nModCount":5},
                                      {"_idRow":1,"_sName":"Digimon World"}]})");
        http.respond(HttpMethod::Get,
                     kBase + "/apiv11/Util/Game/NameMatch?_sName=" + network::percentEncode("Unknown Game"), 200,
                     R"({"_aRecords":[]})");
        http.respond(HttpMethod::Get,
                     kBase + "/apiv11/Mod/Index?_nPerpage=30&_nPage=1&_aFilters%5BGeneric_Game%5D=21999", 200,
                     R"({"_aMetadata":{"_nRecordCount":2},"_aRecords":[
                        {"_idRow":555,"_sModelName":"Mod","_sName":"HD <i>Textures</i>","_aSubmitter":{"_sName":"modder"},
                         "_sVersion":"2.0","_nLikeCount":7,
                         "_aPreviewMedia":{"_aImages":[{"_sBaseUrl":"https://images.gamebanana.com/img/ss/mods","_sFile":"a.jpg","_sFile220":"220-a.jpg"}]}},
                        {"_idRow":556,"_sModelName":"Mod","_sName":"Nsfw","_bIsNsfw":true}]})");
        http.respond(HttpMethod::Get, kBase + "/apiv11/Mod/555/ProfilePage", 200,
                     R"({"_idRow":555,"_sName":"HD Textures","_sText":"<p>Sharper &amp; better</p>","_sVersion":"2.0",
                         "_aGame":{"_idRow":21999},
                         "_aFiles":[{"_idRow":1001,"_sFile":"hd_textures.zip","_nFilesize":4096},
                                    {"_idRow":1002,"_sFile":"other.rar","_nFilesize":10}]})");
        banana.setInstalledGames({{"PPSA24701", "Digimon Story Time Stranger"}, {"PPSA00001", "Unknown Game"}});
    }
};

}  // namespace

TEST_CASE("GameBanana games are matched by name, mods listed as EXPERIMENTAL without NSFW") {
    Fixture f;
    auto games = f.banana.listGames(nullptr);
    REQUIRE(games.ok());
    REQUIRE(games->size() == 1);
    CHECK(games->front().providerGameId == "gamebanana:21999");
    CHECK(games->front().modCount == 5);

    SearchQuery query;
    query.providerGameId = "gamebanana:21999";
    auto page = f.banana.browseMods(query, nullptr);
    REQUIRE(page.ok());
    REQUIRE(page->mods.size() == 1);
    CHECK(page->mods[0].ref == ModRef{"gamebanana", "555"});
    CHECK(page->mods[0].name == "HD Textures");
    CHECK(page->mods[0].author == "modder");
    CHECK(page->mods[0].thumbnailUrl == "https://images.gamebanana.com/img/ss/mods/220-a.jpg");
    CHECK(page->mods[0].compatibility == CompatibilityStatus::Experimental);
    CHECK(page->total == 2);
    query.providerGameId = "gamebanana:../x";
    CHECK_FALSE(f.banana.browseMods(query, nullptr).ok());
}

TEST_CASE("GameBanana details and free downloads") {
    Fixture f;
    (void)f.banana.listGames(nullptr);
    auto details = f.banana.getModDetails({"gamebanana", "555"}, std::nullopt, nullptr);
    REQUIRE(details.ok());
    CHECK(details->description == "Sharper & better");
    CHECK(details->titleIds == std::vector<std::string>{"PPSA24701"});
    CHECK(details->needsConfirmation);
    REQUIRE(details->files.size() == 1);
    CHECK(details->files[0].fileId == "1001");
    CHECK(details->files[0].format == "zip");
    auto ticket = f.banana.resolveDownload({"gamebanana", "555"}, details->files[0], nullptr);
    REQUIRE(ticket.ok());
    CHECK(ticket->url == "https://gamebanana.com/dl/1001");
    CHECK(ticket->expectedSize == 4096);

    downloads::DownloadRequest request;
    request.mod = {"gamebanana", "555"};
    request.displayName = "HD Textures";
    request.modVersion = "2.0";
    request.url = ticket->url;
    request.expectedSize = 4096;
    request.format = mods::ArchiveFormat::Zip;
    CHECK(downloads::validateRequest(request).ok());
    CHECK_FALSE(f.banana.getModDetails({"gamebanana", "1;2"}, std::nullopt, nullptr).ok());
}
