// SPDX-License-Identifier: GPL-3.0-or-later
// Mock-tested against fixtures derived from ShadowMountPlus docs/openapi.yaml (1.7).
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/shadowmount/ShadowMountGameProvider.hpp"

using namespace akeno;
using namespace akeno::shadowmount;
using network::HttpMethod;

namespace {

const std::string kBase = "http://127.0.0.1:10101";

}  // namespace

TEST_CASE("version response exposes API version and capabilities") {
    auto info = parseVersionResponse(test::readFixture("shadowmount/version.json"));
    REQUIRE(info.ok());
    CHECK(info->apiVersion == 1);
    CHECK(info->shadowMountVersion == "1.7");
    CHECK(info->has(capability::kListGames));
    CHECK(info->has(capability::kGameIcon));
    CHECK_FALSE(info->has("does_not_exist"));
}

TEST_CASE("error envelopes become errors with ShadowMount's explanation") {
    auto info = parseVersionResponse(test::readFixture("shadowmount/error_busy.json"));
    REQUIRE_FALSE(info.ok());
    CHECK(info.error().code == ErrorCode::Busy);
    CHECK(info.error().message.find("a game is running") != std::string::npos);
    CHECK(info.error().detail.find("game_active") != std::string::npos);

    CHECK_FALSE(parseVersionResponse("[]").ok());
    CHECK_FALSE(parseVersionResponse("{}").ok());
    CHECK_FALSE(parseVersionResponse("not json").ok());
    CHECK_FALSE(parseVersionResponse(R"({"status":0})").ok());  // no api_version
}

TEST_CASE("game list parsing validates title IDs and keeps good entries") {
    auto list = parseGamesResponse(test::readFixture("shadowmount/games.json"));
    REQUIRE(list.ok());
    REQUIRE(list->games.size() == 4);
    CHECK(list->skipped.size() == 2);

    const Game& folder = list->games[0];
    CHECK(folder.titleId == "PPSA01234");
    CHECK(folder.titleName == "Example Blade");
    CHECK(folder.version == "01.011.000");
    CHECK(folder.sourceType == "folder");
    CHECK(folder.runtimePath == "/system_ex/app/PPSA01234");
    CHECK_FALSE(folder.mounted);

    const Game& image = list->games[1];
    CHECK(image.mounted);
    CHECK(image.titleName == "Another \xC3\x84 Game \xE2\x84\xA2");
    CHECK(image.version.empty());

    const Game& pkg = list->games[2];
    CHECK(pkg.installedPkg);
    CHECK(pkg.runtimePath.empty());
}

TEST_CASE("toGameInfo maps fields") {
    auto list = parseGamesResponse(test::readFixture("shadowmount/games.json"));
    REQUIRE(list.ok());
    games::GameInfo info = toGameInfo(list->games[2]);
    CHECK(info.platform == games::Platform::Ps4);
    CHECK(info.sourceType == games::SourceType::Pkg);
    CHECK(info.installedPkg);
    CHECK_FALSE(info.hasIcon);
    CHECK(info.sizeBytes == std::optional<std::uint64_t>(1073741824));
    CHECK(info.installedMods == 0);
    CHECK_FALSE(info.compatibleMods.has_value());
    CHECK(toGameInfo(list->games[1]).displayVersion() == "unknown");
}

TEST_CASE("storage and settings responses") {
    auto storage = parseStorageResponse(test::readFixture("shadowmount/storage.json"));
    REQUIRE(storage.ok());
    REQUIRE(storage->size() == 2);
    CHECK((*storage)[0].mountPoint == "/user");
    CHECK((*storage)[0].availableBytes == 400000000000ull);
    auto paths = parseSettingsScanPaths(test::readFixture("shadowmount/settings.json"));
    REQUIRE(paths.ok());
    REQUIRE(paths->size() == 2);  // relative path rejected
    CHECK((*paths)[0] == "/mnt/usb0/games");
}

TEST_CASE("the client refuses non-loopback endpoints") {
    test::MockHttpClient http;
    auto lan = ShadowMountClient::create(http, Endpoint{"192.168.1.50", 10101});
    REQUIRE_FALSE(lan.ok());
    CHECK(lan.error().code == ErrorCode::SafetyViolation);
    CHECK(ShadowMountClient::create(http, Endpoint{"127.0.0.1", 10101}).ok());
    CHECK_FALSE(ShadowMountClient::create(http, Endpoint{"127.0.0.1", 0}).ok());
}

TEST_CASE("the client sends JSON POST requests to the documented routes") {
    test::MockHttpClient http;
    http.respond(HttpMethod::Post, kBase + "/api/v1/version", 200, test::readFixture("shadowmount/version.json"));
    http.respond(HttpMethod::Post, kBase + "/api/v1/games", 200, test::readFixture("shadowmount/games.json"));
    auto client = ShadowMountClient::create(http, Endpoint{});
    REQUIRE(client.ok());
    REQUIRE(client->version().ok());
    REQUIRE(client->games().ok());
    auto requests = http.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].method == HttpMethod::Post);
    CHECK(requests[0].body == "{}");
    CHECK(requests[1].body == R"({"include_size":false})");
    bool hasContentType = false;
    for (const auto& [name, value] : requests[0].headers) {
        if (name == "Content-Type" && value == "application/json") hasContentType = true;
    }
    CHECK(hasContentType);
    CHECK(requests[0].maxRedirects == 0);
}

TEST_CASE("icon requests use the validated title ID") {
    test::MockHttpClient http;
    http.respond(HttpMethod::Get, kBase + "/api/v1/games/icon?title_id=PPSA01234&size=thumb", 200, "\x89PNG");
    http.respond(HttpMethod::Get, kBase + "/api/v1/games/icon?title_id=PPSA05678", 404, "{}");
    auto client = ShadowMountClient::create(http, Endpoint{});
    REQUIRE(client.ok());
    CHECK(client->icon("PPSA01234", true).value() == "\x89PNG");
    CHECK(client->icon("PPSA05678", false).error().code == ErrorCode::NotFound);
    CHECK(client->icon("../../x", false).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("connection failures explain that ShadowMountPlus must be running") {
    test::MockHttpClient http;
    http.fail(HttpMethod::Post, kBase + "/api/v1/version", makeError(ErrorCode::Unavailable, "refused"));
    auto client = ShadowMountClient::create(http, Endpoint{});
    REQUIRE(client.ok());
    auto version = client->version();
    REQUIRE_FALSE(version.ok());
    CHECK(version.error().message.find("ShadowMountPlus is not answering on port 10101") != std::string::npos);
}

TEST_CASE("an old ShadowMount without the API is identified") {
    test::MockHttpClient http;
    http.respond(HttpMethod::Post, kBase + "/api/v1/version", 404, "");
    auto client = ShadowMountClient::create(http, Endpoint{});
    REQUIRE(client.ok());
    auto version = client->version();
    REQUIRE_FALSE(version.ok());
    CHECK(version.error().code == ErrorCode::Unsupported);
}

TEST_CASE("provider probe reports capability problems") {
    SUBCASE("compatible") {
        test::MockHttpClient http;
        http.respond(HttpMethod::Post, kBase + "/api/v1/version", 200, test::readFixture("shadowmount/version.json"));
        ShadowMountGameProvider provider(ShadowMountClient::create(http, Endpoint{}).value());
        auto status = provider.probe();
        REQUIRE(status.ok());
        CHECK(status->available);
        CHECK(status->supportsIcons);
        CHECK(status->summary == "connected (ShadowMount+ 1.7, API v1)");
    }
    SUBCASE("game list capability missing") {
        test::MockHttpClient http;
        http.respond(HttpMethod::Post, kBase + "/api/v1/version", 200,
                     test::readFixture("shadowmount/version_no_games.json"));
        ShadowMountGameProvider provider(ShadowMountClient::create(http, Endpoint{}).value());
        auto status = provider.probe();
        REQUIRE(status.ok());
        CHECK_FALSE(status->available);
        CHECK(provider.discoverGames().error().code == ErrorCode::Unsupported);
    }
    SUBCASE("unknown API version is not guessed at") {
        test::MockHttpClient http;
        http.respond(HttpMethod::Post, kBase + "/api/v1/version", 200,
                     test::readFixture("shadowmount/version_api2.json"));
        ShadowMountGameProvider provider(ShadowMountClient::create(http, Endpoint{}).value());
        auto status = provider.probe();
        REQUIRE(status.ok());
        CHECK_FALSE(status->available);
        CHECK(status->summary.find("API version 2 is not supported") != std::string::npos);
    }
}

TEST_CASE("provider discovers games end to end over the mock") {
    test::MockHttpClient http;
    http.respond(HttpMethod::Post, kBase + "/api/v1/version", 200, test::readFixture("shadowmount/version.json"));
    http.respond(HttpMethod::Post, kBase + "/api/v1/games", 200, test::readFixture("shadowmount/games.json"));
    ShadowMountGameProvider provider(ShadowMountClient::create(http, Endpoint{}).value());
    auto games = provider.discoverGames();
    REQUIRE(games.ok());
    CHECK(games->size() == 4);
    CHECK((*games)[0].titleId == "PPSA01234");
}

TEST_CASE("a ShadowMountPlus without game versions (1.7beta4) is completed from param.json") {
    test::TempDir dir;
    const auto folder = dir.path() / "Some Game";
    std::filesystem::create_directories(folder / "sce_sys");
    test::writeText(folder / "sce_sys" / "param.json", R"({"titleId":"PPSA28000","contentVersion":"01.005.000"})");
    // Like 1.7beta4: no "version" key at all.
    const std::string games = R"({"status":0,"count":2,"size_included":false,"games":[
        {"path":")" + folder.string() + R"(","runtime_path":")" + folder.string() + R"(","source_type":"folder",
         "image_type":"","platform":"ps5","title_id":"PPSA28000","content_id":"","title_name":"Without version",
         "last_access_time":"","install_time":"","icon_url":"","app_db_size_bytes":0,"installed":true,"managed":true,
         "mounted":false,"image_backed":false,"source_available":true,"installed_pkg":false},
        {"path":"/data/homebrew/PPSA90001","runtime_path":"","source_type":"folder","image_type":"","platform":"ps5",
         "title_id":"PPSA90001","content_id":"","title_name":"With version","version":"01.011.000",
         "last_access_time":"","install_time":"","icon_url":"","app_db_size_bytes":0,"installed":true,"managed":true,
         "mounted":false,"image_backed":false,"source_available":true,"installed_pkg":false}]})";
    test::MockHttpClient http;
    http.respond(HttpMethod::Post, kBase + "/api/v1/version", 200, test::readFixture("shadowmount/version.json"));
    http.respond(HttpMethod::Post, kBase + "/api/v1/games", 200, games);
    ShadowMountGameProvider provider(ShadowMountClient::create(http, Endpoint{}).value());
    provider.setAppmetaBase(dir.path() / "appmeta");
    auto list = provider.discoverGames();
    REQUIRE(list.ok());
    REQUIRE(list->size() == 2);
    CHECK((*list)[0].version == "01.005.000");
    CHECK((*list)[0].versionSource == (folder / "sce_sys" / "param.json").string());
    CHECK((*list)[1].version == "01.011.000");
    CHECK((*list)[1].versionSource == "ShadowMountPlus");
}
