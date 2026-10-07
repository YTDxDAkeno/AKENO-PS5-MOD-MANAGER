// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Strings.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace akeno::strings {

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string_view trim(std::string_view text) noexcept {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    return text;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

namespace {

// Returns the length of a valid UTF-8 sequence starting at `text[pos]`, or 0 if invalid.
std::size_t utf8SequenceLength(std::string_view text, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (lead < 0x80) {
        return 1;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2;
        codepoint = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        codepoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        codepoint = lead & 0x07;
    } else {
        return 0;
    }
    if (pos + length > text.size()) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto cont = static_cast<unsigned char>(text[pos + i]);
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
        codepoint = (codepoint << 6) | (cont & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    static constexpr std::array<std::uint32_t, 5> kMinimum{0, 0, 0x80, 0x800, 0x10000};
    if (codepoint < kMinimum[length] || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0;
    }
    return length;
}

}  // namespace

std::string truncateUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    std::size_t cut = maxBytes;
    // Step back over continuation bytes so the cut lands on a sequence boundary.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.substr(0, cut));
}

std::string sanitizeForDisplay(std::string_view text, std::size_t maxBytes) {
    std::string out;
    out.reserve(text.size() < maxBytes ? text.size() : maxBytes);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t length = utf8SequenceLength(text, pos);
        std::string_view piece;
        static constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD
        if (length == 0) {
            piece = kReplacement;
            length = 1;
        } else if (length == 1) {
            const auto c = static_cast<unsigned char>(text[pos]);
            if (c < 0x20 || c == 0x7F) {
                piece = (c == '\t' || c == '\n' || c == '\r') ? std::string_view(" ") : kReplacement;
            } else {
                piece = text.substr(pos, 1);
            }
        } else {
            piece = text.substr(pos, length);
        }
        if (out.size() + piece.size() > maxBytes) {
            break;
        }
        out.append(piece);
        pos += length;
    }
    return out;
}

std::string formatBytes(std::uint64_t bytes) {
    static constexpr std::array<const char*, 5> kUnits{"B", "KB", "MB", "GB", "TB"};
    if (bytes < 1000) {
        return std::to_string(bytes) + " B";
    }
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < kUnits.size()) {
        value /= 1000.0;
        ++unit;
    }
    std::array<char, 32> buffer{};
    if (value >= 100.0) {
        std::snprintf(buffer.data(), buffer.size(), "%.0f %s", value, kUnits[unit]);
    } else {
        std::snprintf(buffer.data(), buffer.size(), "%.1f %s", value, kUnits[unit]);
    }
    return buffer.data();
}

namespace {

std::tm utcNow() {
    std::time_t now = std::time(nullptr);
    std::tm parts{};
    gmtime_r(&now, &parts);
    return parts;
}

}  // namespace

std::string utcTimestamp() {
    std::tm parts = utcNow();
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer.data();
}

std::string utcTimestampCompact() {
    std::tm parts = utcNow();
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y%m%d-%H%M%S", &parts);
    return buffer.data();
}

}  // namespace akeno::strings
