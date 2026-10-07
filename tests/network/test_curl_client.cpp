// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the real libcurl client against a loopback server, the same transport used for
// the ShadowMountPlus API.
#include "Doctest.hpp"

#include "LocalHttpServer.hpp"
#include "akeno/network/CurlHttpClient.hpp"

using namespace akeno;
using namespace akeno::network;

namespace {

CurlGlobal& curlGlobal() {
    static CurlGlobal global;
    return global;
}

std::string baseUrl(const test::LocalHttpServer& server) {
    return "http://127.0.0.1:" + std::to_string(server.port());
}

}  // namespace

TEST_CASE("POST JSON round trip over loopback") {
    REQUIRE(curlGlobal().ok());
    test::LocalHttpServer server([](const test::ReceivedRequest& request) {
        test::CannedResponse response;
        response.body = R"({"status":0,"echo":)" + std::to_string(request.body.size()) + "}";
        return response;
    });
    REQUIRE(server.ok());
    CurlHttpClient client({});
    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = baseUrl(server) + "/api/v1/version";
    request.headers = {{"Content-Type", "application/json"}};
    request.body = "{}";
    auto response = client.send(request);
    REQUIRE(response.ok());
    CHECK(response->status == 200);
    CHECK(response->body == R"({"status":0,"echo":2})");
    CHECK(response->header("content-type") == "application/json");

    auto received = server.received();
    REQUIRE(received.size() == 1);
    CHECK(received[0].method == "POST");
    CHECK(received[0].target == "/api/v1/version");
    CHECK(received[0].body == "{}");
}

TEST_CASE("an oversized declared body is refused before download") {
    REQUIRE(curlGlobal().ok());
    test::LocalHttpServer server([](const test::ReceivedRequest&) {
        test::CannedResponse response;
        response.body = std::string(10000, 'x');
        return response;
    });
    CurlHttpClient client({});
    HttpRequest request;
    request.url = baseUrl(server) + "/big";
    request.maxResponseBytes = 1000;
    auto response = client.send(request);
    REQUIRE_FALSE(response.ok());
    CHECK(response.error().code == ErrorCode::ResponseTooLarge);
}

TEST_CASE("an oversized streamed body without Content-Length is cut off") {
    REQUIRE(curlGlobal().ok());
    test::LocalHttpServer server([](const test::ReceivedRequest&) {
        test::CannedResponse response;
        response.body = std::string(50000, 'x');
        response.omitContentLength = true;
        return response;
    });
    CurlHttpClient client({});
    HttpRequest request;
    request.url = baseUrl(server) + "/stream";
    request.maxResponseBytes = 1000;
    auto response = client.send(request);
    REQUIRE_FALSE(response.ok());
    CHECK(response.error().code == ErrorCode::ResponseTooLarge);
}

TEST_CASE("HTTP error statuses are returned, not turned into transport errors") {
    REQUIRE(curlGlobal().ok());
    test::LocalHttpServer server([](const test::ReceivedRequest&) {
        test::CannedResponse response;
        response.status = 409;
        response.body = R"({"status":16,"error":"busy"})";
        return response;
    });
    CurlHttpClient client({});
    HttpRequest request;
    request.url = baseUrl(server) + "/x";
    auto response = client.send(request);
    REQUIRE(response.ok());
    CHECK(response->status == 409);
    CHECK_FALSE(response->isSuccess());
}

TEST_CASE("a slow server hits the total timeout") {
    REQUIRE(curlGlobal().ok());
    test::LocalHttpServer server([](const test::ReceivedRequest&) {
        test::CannedResponse response;
        response.delayMs = 1500;
        return response;
    });
    CurlHttpClient client({});
    HttpRequest request;
    request.url = baseUrl(server) + "/slow";
    request.totalTimeoutMs = 300;
    auto response = client.send(request);
    REQUIRE_FALSE(response.ok());
    CHECK(response.error().code == ErrorCode::Timeout);
}

TEST_CASE("nothing listening gives a clear connection error") {
    REQUIRE(curlGlobal().ok());
    std::uint16_t port = 0;
    {
        test::LocalHttpServer server([](const test::ReceivedRequest&) { return test::CannedResponse{}; });
        port = server.port();
    }
    CurlHttpClient client({});
    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(port) + "/";
    request.connectTimeoutMs = 1000;
    auto response = client.send(request);
    REQUIRE_FALSE(response.ok());
    CHECK((response.error().code == ErrorCode::Unavailable || response.error().code == ErrorCode::Timeout));
}

TEST_CASE("cancellation aborts a transfer") {
    REQUIRE(curlGlobal().ok());
    test::LocalHttpServer server([](const test::ReceivedRequest&) {
        test::CannedResponse response;
        response.delayMs = 2000;
        return response;
    });
    CurlHttpClient client({});
    HttpRequest request;
    request.url = baseUrl(server) + "/slow";
    CancellationToken token;
    token.cancel();
    auto response = client.send(request, &token);
    REQUIRE_FALSE(response.ok());
    CHECK(response.error().code == ErrorCode::Cancelled);
}

TEST_CASE("policy is enforced before any connection is made") {
    REQUIRE(curlGlobal().ok());
    CurlHttpClient client({});
    HttpRequest request;
    request.url = "http://203.0.113.10/";
    auto response = client.send(request);
    REQUIRE_FALSE(response.ok());
    CHECK(response.error().code == ErrorCode::SafetyViolation);
}
