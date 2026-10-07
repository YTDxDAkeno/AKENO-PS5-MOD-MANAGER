// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/downloads/DownloadTypes.hpp"
#include "akeno/providers/NexusProvider.hpp"

using namespace akeno;
using namespace akeno::providers;
using network::HttpMethod;

namespace {

const std::string kBase = "https://api.nexusmods.com";
const std::string kKey = "abcdefghijklmnopqrstuvwxyz0123456789ABCD==";

const char* kMod = R"({"mod_id":42,"name":"Better <b>Outfits</b>","summary":"Nicer &amp; brighter","author":"someone",
"version":"1.2","picture_url":"https://staticdelivery.nexusmods.com/mods/1/images/42.png","available":true,
"status":"published","contains_adult_content":false,"endorsement_count":10,"updated_time":"2026-09-01"})";

struct Fixture {
    test::MockHttpClient http;
    NexusProvider nexus{http, kKey, kBase};

    Fixture() {
        http.respond(HttpMethod::Get, kBase + "/v1/users/validate.json", 200, R"({"is_premium":true,"name":"x"})");
        http.respond(HttpMethod::Get, kBase + "/v1/games.json", 200,
                     R"([{"domain_name":"digimonstorytimestranger","name":"Digimon Story: Time Stranger","mods":12},
                         {"domain_name":"skyrim","name":"Skyrim","mods":99999}])");
        const std::string list = std::string("[") + kMod +
                                 R"(,{"mod_id":43,"name":"Adult","contains_adult_content":true},{"mod_id":44,"name":"Gone","available":false}])";
        for (const char* name : {"trending", "latest_updated", "latest_added"}) {
            http.respond(HttpMethod::Get,
                         kBase + "/v1/games/digimonstorytimestranger/mods/" + name + ".json", 200, list);
        }
        http.respond(HttpMethod::Get, kBase + "/v1/games/digimonstorytimestranger/mods/42.json", 200, kMod);
        http.respond(HttpMethod::Get, kBase + "/v1/games/digimonstorytimestranger/mods/42/files.json", 200,
                     R"({"files":[{"file_id":7,"name":"Old","category_name":"OLD_VERSION","size_in_bytes":10,"file_name":"a.zip"},
                                  {"file_id":8,"name":"Main","category_name":"MAIN","size_in_bytes":2048,"file_name":"Better-42-1-2.7z","version":"1.2"},
                                  {"file_id":9,"name":"Rar","category_name":"MAIN","size_in_bytes":99,"file_name":"x.rar"}]})");
        nexus.setInstalledGames({{"PPSA24701", "Digimon Story Time Stranger"}, {"PPSA01325", "ASTRO's PLAYROOM"}});
    }
};

}  // namespace

TEST_CASE("Nexus games are matched to installed games by name") {
    CHECK(normalizeGameName("MONSTER HUNTER STORIES 3: TWISTED REFLECTION") ==
          normalizeGameName("Monster Hunter Stories 3 Twisted Reflection"));
    Fixture f;
    auto games = f.nexus.listGames(nullptr);
    REQUIRE(games.ok());
    REQUIRE(games->size() == 1);
    CHECK(games->front().providerGameId == "nexus:digimonstorytimestranger");
    CHECK(games->front().titleIds == std::vector<std::string>{"PPSA24701"});
    CHECK(f.nexus.premium() == true);
    // The key goes only into the apikey header.
    for (const auto& request : f.http.requests()) {
        CHECK(request.url.find(kKey) == std::string::npos);
        bool hasKey = false;
        for (const auto& [name, value] : request.headers) hasKey = hasKey || (name == "apikey" && value == kKey);
        CHECK(hasKey);
    }
}

TEST_CASE("Nexus mods are listed as EXPERIMENTAL plain text; adult and removed mods are left out") {
    Fixture f;
    SearchQuery query;
    query.providerGameId = "nexus:digimonstorytimestranger";
    auto page = f.nexus.browseMods(query, nullptr);
    REQUIRE(page.ok());
    REQUIRE(page->mods.size() == 1);
    const ModSummary& mod = page->mods.front();
    CHECK(mod.ref == ModRef{"nexus", "digimonstorytimestranger/42"});
    CHECK(mod.name == "Better Outfits");
    CHECK(mod.shortDescription == "Nicer & brighter");
    CHECK(mod.compatibility == CompatibilityStatus::Experimental);
    query.text = "outfit";
    CHECK(f.nexus.searchMods(query, nullptr)->mods.size() == 1);
    query.text = "zzz";
    CHECK(f.nexus.searchMods(query, nullptr)->mods.empty());
    query.providerGameId = "nexus:../etc";
    CHECK_FALSE(f.nexus.browseMods(query, nullptr).ok());
}

TEST_CASE("Nexus details need confirmation and list only zip and 7z files") {
    Fixture f;
    auto details = f.nexus.getModDetails({"nexus", "digimonstorytimestranger/42"}, GameContext{"PPSA24701", ""}, nullptr);
    REQUIRE(details.ok());
    CHECK(details->needsConfirmation);
    CHECK(details->installable);
    CHECK(details->risk == "HIGH");
    REQUIRE(details->files.size() == 1);
    CHECK(details->files[0].fileId == "8");
    CHECK(details->files[0].format == "7z");
    CHECK(details->files[0].sizeBytes == 2048);
    CHECK(details->files[0].primary);
    CHECK_FALSE(f.nexus.getModDetails({"nexus", "x/../../42"}, std::nullopt, nullptr).ok());
}

TEST_CASE("Nexus downloads: premium links are used, others are refused with an explanation") {
    Fixture f;
    ModFile file;
    file.fileId = "8";
    file.sizeBytes = 2048;
    file.format = "7z";
    const std::string link = kBase + "/v1/games/digimonstorytimestranger/mods/42/files/8/download_link.json";
    f.http.respond(HttpMethod::Get, link, 200,
                   R"([{"name":"Nexus CDN","short_name":"Nexus CDN","URI":"https://cf-files.nexusmods.com/x/42/Better.7z?md5=a"}])");
    auto ticket = f.nexus.resolveDownload({"nexus", "digimonstorytimestranger/42"}, file, nullptr);
    REQUIRE(ticket.ok());
    CHECK(ticket->url.rfind("https://cf-files.nexusmods.com/", 0) == 0);
    CHECK(ticket->expectedSize == 2048);
    CHECK(ticket->headers.empty());

    // A download request without a checksum is accepted only for Nexus.
    downloads::DownloadRequest request;
    request.mod = {"nexus", "digimonstorytimestranger/42"};
    request.displayName = "Better Outfits";
    request.modVersion = "1.2";
    request.url = ticket->url;
    request.expectedSize = 2048;
    request.format = mods::ArchiveFormat::SevenZip;
    CHECK(downloads::validateRequest(request).ok());
    request.mod.providerId = "akeno-catalogue";
    CHECK_FALSE(downloads::validateRequest(request).ok());

    f.http.respond(HttpMethod::Get, link, 403, R"({"message":"premium only"})");
    auto refused = f.nexus.resolveDownload({"nexus", "digimonstorytimestranger/42"}, file, nullptr);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Unsupported);
    CHECK(refused.error().message.find("Premium") != std::string::npos);
}

TEST_CASE("only plausible Nexus API keys are used") {
    CHECK(isPlausibleNexusKey(kKey));
    CHECK_FALSE(isPlausibleNexusKey("short"));
    CHECK_FALSE(isPlausibleNexusKey("has spaces in it but is long enough"));
}
