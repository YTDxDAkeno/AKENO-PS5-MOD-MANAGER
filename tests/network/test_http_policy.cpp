// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/network/Http.hpp"

using namespace akeno;
using namespace akeno::network;

namespace {

HttpRequest get(std::string url) {
    HttpRequest request;
    request.url = std::move(url);
    return request;
}

}  // namespace

TEST_CASE("https is allowed everywhere, plain http only on loopback") {
    CHECK(checkRequestPolicy(get("https://example.com/")).ok());
    CHECK(checkRequestPolicy(get("http://127.0.0.1:10101/api/v1/version")).ok());
    CHECK(checkRequestPolicy(get("http://localhost/")).ok());
    auto lan = checkRequestPolicy(get("http://192.168.1.50:10101/api/v1/version"));
    REQUIRE_FALSE(lan.ok());
    CHECK(lan.error().code == ErrorCode::SafetyViolation);
    CHECK_FALSE(checkRequestPolicy(get("http://example.com/")).ok());
}

TEST_CASE("header injection is refused") {
    HttpRequest request = get("https://example.com/");
    request.headers = {{"X-Test", "ok\r\nInjected: yes"}};
    CHECK_FALSE(checkRequestPolicy(request).ok());
    request.headers = {{"Bad:Name", "x"}};
    CHECK_FALSE(checkRequestPolicy(request).ok());
    request.headers = {{"Accept", "application/json"}};
    CHECK(checkRequestPolicy(request).ok());
}

TEST_CASE("limits must be sane") {
    HttpRequest request = get("https://example.com/");
    request.maxResponseBytes = 0;
    CHECK_FALSE(checkRequestPolicy(request).ok());
    request = get("https://example.com/");
    request.maxRedirects = 50;
    CHECK_FALSE(checkRequestPolicy(request).ok());
}

TEST_CASE("response header lookup is case-insensitive") {
    HttpResponse response;
    response.headers = {{"Content-Type", "application/json"}};
    CHECK(response.header("content-type") == "application/json");
    CHECK(response.header("missing").empty());
}
