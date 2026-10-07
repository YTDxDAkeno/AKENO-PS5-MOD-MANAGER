// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/platform/Platform.hpp"

using akeno::platform::formatFirmwareVersion;

TEST_CASE("firmware words decode like the SDK's offset tables") {
    CHECK(formatFirmwareVersion(0x12200000) == std::optional<std::string>("12.20"));
    CHECK(formatFirmwareVersion(0x12020000) == std::optional<std::string>("12.02"));
    CHECK(formatFirmwareVersion(0x01050000) == std::optional<std::string>("1.05"));
    CHECK(formatFirmwareVersion(0x13420000) == std::optional<std::string>("13.42"));
    CHECK(formatFirmwareVersion(0x07610000) == std::optional<std::string>("7.61"));
}

TEST_CASE("invalid firmware words are reported as unknown") {
    CHECK_FALSE(formatFirmwareVersion(0).has_value());
    CHECK_FALSE(formatFirmwareVersion(0x1A000000).has_value());
    CHECK_FALSE(formatFirmwareVersion(0x120F0000).has_value());
    CHECK_FALSE(formatFirmwareVersion(0x00500000).has_value());
}

TEST_CASE("the host platform never claims to be a console") {
    auto platform = akeno::platform::createPlatform();
    CHECK_FALSE(platform->isConsole());
    CHECK_FALSE(platform->firmware().known);
}
