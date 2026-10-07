// SPDX-License-Identifier: GPL-3.0-or-later
// Masks credentials before text reaches any log sink.
#pragma once

#include <string>
#include <string_view>

namespace akeno::logging {

// Replaces the value following a sensitive key with "[REDACTED]". Recognised forms:
//   api_key=VALUE&...           (query strings, ini)
//   "token": "VALUE"            (JSON)
//   Authorization: Bearer VALUE (HTTP headers)
// Keys are matched case-insensitively: apikey, api_key, api-key, x-api-key, token,
// access_token, refresh_token, id_token, password, passwd, secret, client_secret,
// authorization, cookie, session.
std::string redactSecrets(std::string_view text);

}  // namespace akeno::logging
