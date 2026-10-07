// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/core/Result.hpp"
#include "akeno/core/Strings.hpp"

using namespace akeno;

TEST_CASE("truncateUtf8 never splits a multi-byte sequence") {
    const std::string text = "ab\xC3\xA4" "cd";  // "abäcd"
    CHECK(strings::truncateUtf8(text, 10) == text);
    CHECK(strings::truncateUtf8(text, 4) == "ab\xC3\xA4");
    CHECK(strings::truncateUtf8(text, 3) == "ab");
    CHECK(strings::truncateUtf8(text, 2) == "ab");
    CHECK(strings::truncateUtf8(text, 0).empty());
}

TEST_CASE("sanitizeForDisplay removes control characters and invalid UTF-8") {
    CHECK(strings::sanitizeForDisplay("plain", 100) == "plain");
    CHECK(strings::sanitizeForDisplay("line\nbreak\ttab", 100) == "line break tab");
    CHECK(strings::sanitizeForDisplay(std::string("nul\0x", 5), 100) == "nul\xEF\xBF\xBDx");
    CHECK(strings::sanitizeForDisplay("bad\xFF", 100) == "bad\xEF\xBF\xBD");
    // Overlong encoding of '/' must not survive.
    CHECK(strings::sanitizeForDisplay("\xC0\xAF", 100) == "\xEF\xBF\xBD\xEF\xBF\xBD");
    // Surrogates are invalid in UTF-8.
    CHECK(strings::sanitizeForDisplay("\xED\xA0\x80", 100).find("\xED\xA0\x80") == std::string::npos);
    CHECK(strings::sanitizeForDisplay("\xE2\x84\xA2", 100) == "\xE2\x84\xA2");  // ™ stays
    CHECK(strings::sanitizeForDisplay("abcdef", 3) == "abc");
}

TEST_CASE("formatBytes uses decimal units") {
    CHECK(strings::formatBytes(0) == "0 B");
    CHECK(strings::formatBytes(999) == "999 B");
    CHECK(strings::formatBytes(1000) == "1.0 KB");
    CHECK(strings::formatBytes(438000000) == "438 MB");
    CHECK(strings::formatBytes(1200000000) == "1.2 GB");
}

TEST_CASE("split, trim and case helpers") {
    auto parts = strings::split("a/b//c", '/');
    REQUIRE(parts.size() == 4);
    CHECK(parts[2].empty());
    CHECK(strings::trim("  x y \t\n") == "x y");
    CHECK(strings::equalsIgnoreCaseAscii("Content-Length", "content-length"));
    CHECK_FALSE(strings::equalsIgnoreCaseAscii("abc", "abd"));
    CHECK(strings::startsWith("https://x", "https://"));
    CHECK(strings::endsWith("file.partial", ".partial"));
}

TEST_CASE("Result carries a value or an error") {
    Result<int> good = 5;
    REQUIRE(good.ok());
    CHECK(good.value() == 5);
    Result<int> bad = makeError(ErrorCode::NotFound, "missing", "detail");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::NotFound);
    CHECK(bad.valueOr(7) == 7);
    CHECK(bad.error().describe() == "[not-found] missing (detail)");
    Status fine;
    CHECK(fine.ok());
}
