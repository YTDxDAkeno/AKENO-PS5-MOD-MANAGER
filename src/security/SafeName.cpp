// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/SafeName.hpp"

namespace akeno::security {

namespace {

constexpr std::size_t kMaxComponentBytes = 128;

bool isAllowedChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

}  // namespace

bool isSafeFileComponent(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxComponentBytes || name.front() == '.') {
        return false;
    }
    for (char c : name) {
        if (!isAllowedChar(c)) {
            return false;
        }
    }
    return true;
}

std::string toSafeFileComponent(std::string_view text, std::string_view fallback) {
    std::string out;
    out.reserve(text.size() < kMaxComponentBytes ? text.size() : kMaxComponentBytes);
    for (char c : text) {
        if (out.size() >= kMaxComponentBytes) {
            break;
        }
        if (out.empty() && c == '.') {
            continue;
        }
        out.push_back(isAllowedChar(c) ? c : '_');
    }
    if (out.empty() || !isSafeFileComponent(out)) {
        return std::string(fallback);
    }
    return out;
}

}  // namespace akeno::security
