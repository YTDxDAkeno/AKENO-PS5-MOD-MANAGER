// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "akeno/core/Result.hpp"

namespace akeno::network {

struct Url {
    std::string scheme;   // "http" or "https"
    std::string host;     // lower-case; IPv6 without brackets
    std::uint16_t port = 0;
    std::string target;   // path + optional query, always starts with '/'

    bool isHttps() const noexcept { return scheme == "https"; }
    std::string authority() const;  // host[:port] as it appears in a URL
    std::string toString() const;
};

// Strict parser for absolute http(s) URLs. Rejects user-info ("user:pass@host"), fragments,
// whitespace, control characters, empty hosts and invalid ports.
Result<Url> parseUrl(std::string_view text);

// 127.0.0.0/8, ::1 and "localhost".
bool isLoopbackHost(std::string_view host) noexcept;

// Percent-encodes everything outside the RFC 3986 unreserved set.
std::string percentEncode(std::string_view text);

}  // namespace akeno::network
