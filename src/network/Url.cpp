// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/network/Url.hpp"

#include <array>
#include <charconv>

#include "akeno/core/Strings.hpp"

namespace akeno::network {

namespace {

Error invalid(std::string_view text, std::string_view reason) {
    return makeError(ErrorCode::InvalidArgument, "The web address is not valid.",
                     strings::concat(reason, ": ", strings::sanitizeForDisplay(text, 200)));
}

bool isHostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

bool isIpv6Char(char c) {
    return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == ':' || c == '.';
}

}  // namespace

std::string Url::authority() const {
    std::string text = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    const std::uint16_t defaultPort = isHttps() ? 443 : 80;
    if (port != defaultPort) {
        text += ":" + std::to_string(port);
    }
    return text;
}

std::string Url::toString() const { return scheme + "://" + authority() + target; }

Result<Url> parseUrl(std::string_view text) {
    if (text.size() > 4096) {
        return invalid(text, "too long");
    }
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return invalid(text, "contains whitespace or control characters");
        }
    }
    Url url;
    std::string_view rest;
    if (strings::startsWith(strings::toLowerAscii(text.substr(0, 8)), "https://")) {
        url.scheme = "https";
        url.port = 443;
        rest = text.substr(8);
    } else if (strings::startsWith(strings::toLowerAscii(text.substr(0, 7)), "http://")) {
        url.scheme = "http";
        url.port = 80;
        rest = text.substr(7);
    } else {
        return invalid(text, "only http and https are supported");
    }
    if (rest.find('#') != std::string_view::npos) {
        return invalid(text, "fragments are not allowed");
    }
    std::size_t targetStart = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, targetStart);
    std::string_view target = targetStart == std::string_view::npos ? std::string_view{} : rest.substr(targetStart);
    if (authority.find('@') != std::string_view::npos) {
        return invalid(text, "credentials in URLs are not allowed");
    }
    std::string_view hostPart;
    std::string_view portPart;
    if (!authority.empty() && authority.front() == '[') {
        std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return invalid(text, "unterminated IPv6 address");
        }
        hostPart = authority.substr(1, close - 1);
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return invalid(text, "unexpected text after IPv6 address");
            portPart = after.substr(1);
        }
        url.host = strings::toLowerAscii(hostPart);
        for (char c : url.host) {
            if (!isIpv6Char(c)) return invalid(text, "invalid IPv6 address");
        }
    } else {
        std::size_t colon = authority.rfind(':');
        hostPart = colon == std::string_view::npos ? authority : authority.substr(0, colon);
        if (colon != std::string_view::npos) portPart = authority.substr(colon + 1);
        url.host = strings::toLowerAscii(hostPart);
        for (char c : url.host) {
            if (!isHostChar(c)) return invalid(text, "invalid host name");
        }
    }
    if (url.host.empty() || url.host.size() > 253) {
        return invalid(text, "missing or overlong host");
    }
    if (!portPart.empty() || (authority.size() > 0 && authority.back() == ':')) {
        unsigned value = 0;
        auto [ptr, ec] = std::from_chars(portPart.data(), portPart.data() + portPart.size(), value);
        if (portPart.empty() || ec != std::errc() || ptr != portPart.data() + portPart.size() || value == 0 ||
            value > 65535) {
            return invalid(text, "invalid port");
        }
        url.port = static_cast<std::uint16_t>(value);
    }
    if (target.empty()) {
        url.target = "/";
    } else if (target.front() == '?') {
        url.target = "/" + std::string(target);
    } else {
        url.target = std::string(target);
    }
    return url;
}

bool isLoopbackHost(std::string_view host) noexcept {
    if (host == "localhost" || host == "::1") {
        return true;
    }
    // 127.0.0.0/8 in dotted-quad form only.
    if (!strings::startsWith(host, "127.")) {
        return false;
    }
    int parts = 0;
    std::size_t start = 0;
    while (start <= host.size()) {
        std::size_t dot = host.find('.', start);
        std::string_view part = host.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (part.empty() || part.size() > 3) return false;
        unsigned value = 0;
        for (char c : part) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value > 255) return false;
        ++parts;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts == 4;
}

std::string percentEncode(std::string_view text) {
    static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string out;
    out.reserve(text.size() * 3);
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

}  // namespace akeno::network
