// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace akeno::strings {

std::string toLowerAscii(std::string_view text);
bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept;
bool startsWith(std::string_view text, std::string_view prefix) noexcept;
bool endsWith(std::string_view text, std::string_view suffix) noexcept;
std::string_view trim(std::string_view text) noexcept;
std::vector<std::string> split(std::string_view text, char separator);

// Truncates to at most `maxBytes` without splitting a UTF-8 sequence.
std::string truncateUtf8(std::string_view text, std::size_t maxBytes);

// Replaces control characters and invalid UTF-8 so a string is safe to render and log.
std::string sanitizeForDisplay(std::string_view text, std::size_t maxBytes);

// "1.2 GB", "438 MB", "12 KB", "512 B" (decimal units, as shown by the console).
std::string formatBytes(std::uint64_t bytes);

// UTC timestamp, ISO-8601 ("2026-10-07T11:22:33Z").
std::string utcTimestamp();
// Compact form usable in file names ("20261007-112233").
std::string utcTimestampCompact();

template <typename... Parts>
std::string concat(const Parts&... parts) {
    std::ostringstream stream;
    (stream << ... << parts);
    return stream.str();
}

}  // namespace akeno::strings
