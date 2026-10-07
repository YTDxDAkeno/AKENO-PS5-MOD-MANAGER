// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/logging/Redactor.hpp"

#include <array>

#include "akeno/core/Strings.hpp"

namespace akeno::logging {

namespace {

constexpr std::string_view kMask = "[REDACTED]";

struct SensitiveKey {
    std::string_view name;
    bool valueRunsToEndOfLine;   // header-style values such as "Authorization: Bearer x y"
    bool queryParameterOnly;     // only when preceded by '?' or '&' (generic names like "key")
};

// Longest names first so "access_token" wins over "token".
constexpr std::array<SensitiveKey, 19> kKeys{{
    {"proxy-authorization", true, false},
    {"authorization", true, false},
    {"client_secret", false, false},
    {"refresh_token", false, false},
    {"access_token", false, false},
    {"set-cookie", true, false},
    {"x-api-key", false, false},
    {"sessionid", false, false},
    {"id_token", false, false},
    {"password", false, false},
    {"api_key", false, false},
    {"api-key", false, false},
    {"nxm_key", false, false},
    {"session", false, false},
    {"apikey", false, false},
    {"cookie", true, false},
    {"passwd", false, false},
    {"secret", false, false},
    {"token", false, false},
}};

constexpr SensitiveKey kQueryKey{"key", false, true};

bool isKeyChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// Tries to match a sensitive key at `pos`. Returns the matched key or nullptr.
const SensitiveKey* matchKey(std::string_view lower, std::size_t pos) {
    if (pos > 0 && isKeyChar(lower[pos - 1])) {
        return nullptr;
    }
    auto matches = [&](const SensitiveKey& key) {
        if (lower.compare(pos, key.name.size(), key.name) != 0) {
            return false;
        }
        std::size_t end = pos + key.name.size();
        if (end < lower.size() && isKeyChar(lower[end])) {
            return false;
        }
        if (key.queryParameterOnly && (pos == 0 || (lower[pos - 1] != '?' && lower[pos - 1] != '&'))) {
            return false;
        }
        return true;
    };
    for (const auto& key : kKeys) {
        if (matches(key)) {
            return &key;
        }
    }
    if (matches(kQueryKey)) {
        return &kQueryKey;
    }
    return nullptr;
}

}  // namespace

std::string redactSecrets(std::string_view text) {
    const std::string lower = strings::toLowerAscii(text);
    std::string out;
    out.reserve(text.size());

    std::size_t copiedUpTo = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const SensitiveKey* key = matchKey(lower, pos);
        if (key == nullptr) {
            ++pos;
            continue;
        }
        std::size_t cursor = pos + key->name.size();
        if (cursor < text.size() && (text[cursor] == '"' || text[cursor] == '\'')) {
            ++cursor;  // closing quote of a quoted key
        }
        while (cursor < text.size() && isSpace(text[cursor])) ++cursor;
        if (cursor >= text.size() || (text[cursor] != ':' && text[cursor] != '=')) {
            pos += key->name.size();
            continue;
        }
        ++cursor;
        while (cursor < text.size() && isSpace(text[cursor])) ++cursor;

        char quote = '\0';
        if (cursor < text.size() && (text[cursor] == '"' || text[cursor] == '\'')) {
            quote = text[cursor];
            ++cursor;
        }
        // Keep an authentication scheme word visible: "Bearer [REDACTED]".
        for (std::string_view scheme : {"bearer ", "basic ", "token "}) {
            if (lower.compare(cursor, scheme.size(), scheme) == 0) {
                cursor += scheme.size();
                break;
            }
        }
        std::size_t valueStart = cursor;
        std::size_t valueEnd = cursor;
        if (quote != '\0') {
            while (valueEnd < text.size() && text[valueEnd] != quote) {
                valueEnd += (text[valueEnd] == '\\' && valueEnd + 1 < text.size()) ? 2 : 1;
            }
            if (valueEnd > text.size()) valueEnd = text.size();
        } else if (key->valueRunsToEndOfLine) {
            while (valueEnd < text.size() && text[valueEnd] != '\n' && text[valueEnd] != '\r') ++valueEnd;
        } else {
            while (valueEnd < text.size()) {
                char c = text[valueEnd];
                if (isSpace(c) || c == '&' || c == ',' || c == ';' || c == '}' || c == ']' || c == '"' ||
                    c == '\'' || c == '\n' || c == '\r') {
                    break;
                }
                ++valueEnd;
            }
        }
        if (valueEnd == valueStart) {
            pos = cursor;
            continue;
        }
        out.append(text.substr(copiedUpTo, valueStart - copiedUpTo));
        out.append(kMask);
        copiedUpTo = valueEnd;
        pos = valueEnd;
    }
    out.append(text.substr(copiedUpTo));
    return out;
}

}  // namespace akeno::logging
