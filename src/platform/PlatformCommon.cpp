// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdio>

#include "akeno/platform/Platform.hpp"

namespace akeno::platform {

std::optional<std::string> formatFirmwareVersion(std::uint32_t raw) {
    if (raw == 0) {
        return std::nullopt;
    }
    const unsigned major = (raw >> 24) & 0xFFu;
    const unsigned minor = (raw >> 16) & 0xFFu;
    auto isBcd = [](unsigned byte) { return (byte >> 4) <= 9 && (byte & 0xFu) <= 9; };
    if (!isBcd(major) || !isBcd(minor) || major == 0) {
        return std::nullopt;
    }
    const unsigned majorDecimal = (major >> 4) * 10 + (major & 0xFu);
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%u.%02x", majorDecimal, minor);
    return std::string(buffer.data());
}

}  // namespace akeno::platform
