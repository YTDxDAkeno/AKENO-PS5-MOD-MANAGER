// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/core/Json.hpp"

using namespace akeno;

TEST_CASE("parseBounded rejects oversized input before parsing") {
    std::string big(1000, ' ');
    auto result = json::parseBounded(big, 100);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::ResponseTooLarge);
}

TEST_CASE("parseBounded rejects malformed JSON without throwing") {
    for (const char* text : {"{", "[1,2", "{\"a\":}", "nul", "\"unterminated", "{} trailing"}) {
        auto result = json::parseBounded(text, 1000);
        CHECK_FALSE(result.ok());
    }
}

TEST_CASE("parseBounded enforces the nesting limit") {
    std::string deep(200, '[');
    deep += std::string(200, ']');
    auto result = json::parseBounded(deep, 10000, 64);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::ParseError);

    std::string shallow = "[[[[1]]]]";
    CHECK(json::parseBounded(shallow, 1000, 64).ok());
}

TEST_CASE("typed accessors return nullopt on type mismatch") {
    auto doc = json::parseBounded(R"({"s":"text","i":42,"b":true,"neg":-1,"big":18446744073709551615,"a":[1],"o":{}})",
                                  1000);
    REQUIRE(doc.ok());
    const auto& d = doc.value();
    CHECK(json::getString(d, "s") == std::optional<std::string>("text"));
    CHECK_FALSE(json::getString(d, "i").has_value());
    CHECK(json::getInt(d, "i") == std::optional<std::int64_t>(42));
    CHECK(json::getInt(d, "neg") == std::optional<std::int64_t>(-1));
    CHECK_FALSE(json::getInt(d, "big").has_value());
    CHECK_FALSE(json::getInt(d, "s").has_value());
    CHECK(json::getBool(d, "b") == std::optional<bool>(true));
    CHECK(json::getArray(d, "a") != nullptr);
    CHECK(json::getArray(d, "o") == nullptr);
    CHECK(json::getObject(d, "o") != nullptr);
    CHECK_FALSE(json::getString(d, "missing").has_value());
}

TEST_CASE("displayString sanitizes and falls back") {
    auto doc = json::parseBounded(R"({"name":"bad\u0001name","empty":""})", 1000);
    REQUIRE(doc.ok());
    CHECK(json::displayString(doc.value(), "name") == "bad\xEF\xBF\xBDname");
    CHECK(json::displayString(doc.value(), "empty", "fallback") == "fallback");
    CHECK(json::displayString(doc.value(), "missing", "fallback") == "fallback");
}
