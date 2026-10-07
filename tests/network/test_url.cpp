// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/network/Url.hpp"

using namespace akeno::network;

TEST_CASE("parseUrl accepts ordinary http(s) URLs") {
    auto url = parseUrl("https://raw.githubusercontent.com/owner/repo/main/catalog.json?x=1");
    REQUIRE(url.ok());
    CHECK(url->scheme == "https");
    CHECK(url->host == "raw.githubusercontent.com");
    CHECK(url->port == 443);
    CHECK(url->target == "/owner/repo/main/catalog.json?x=1");

    auto local = parseUrl("http://127.0.0.1:10101/api/v1/version");
    REQUIRE(local.ok());
    CHECK(local->port == 10101);
    CHECK(local->toString() == "http://127.0.0.1:10101/api/v1/version");

    auto bare = parseUrl("HTTPS://Example.COM");
    REQUIRE(bare.ok());
    CHECK(bare->host == "example.com");
    CHECK(bare->target == "/");

    auto v6 = parseUrl("http://[::1]:8080/x");
    REQUIRE(v6.ok());
    CHECK(v6->host == "::1");
    CHECK(v6->toString() == "http://[::1]:8080/x");
}

TEST_CASE("parseUrl rejects dangerous or malformed URLs") {
    for (const char* text : {"ftp://example.com/file", "file:///etc/passwd", "https://user:pass@example.com/",
                             "https://example.com/a b", "https://example.com/#frag", "https://", "https://:443/",
                             "https://example.com:0/", "https://example.com:99999/", "https://example.com:/",
                             "https://exa_mple.com/", "javascript:alert(1)", "https://example.com\r\nX: y"}) {
        CAPTURE(text);
        CHECK_FALSE(parseUrl(text).ok());
    }
}

TEST_CASE("loopback detection") {
    CHECK(isLoopbackHost("127.0.0.1"));
    CHECK(isLoopbackHost("127.255.0.9"));
    CHECK(isLoopbackHost("localhost"));
    CHECK(isLoopbackHost("::1"));
    CHECK_FALSE(isLoopbackHost("127.0.0.1.evil.com"));
    CHECK_FALSE(isLoopbackHost("128.0.0.1"));
    CHECK_FALSE(isLoopbackHost("192.168.1.50"));
    CHECK_FALSE(isLoopbackHost("127.0.0"));
    CHECK_FALSE(isLoopbackHost("127.0.0.256"));
    CHECK_FALSE(isLoopbackHost("0.0.0.0"));
}

TEST_CASE("percentEncode leaves unreserved characters") {
    CHECK(percentEncode("Stellar Blade/1") == "Stellar%20Blade%2F1");
    CHECK(percentEncode("a-b_c.d~e") == "a-b_c.d~e");
}
