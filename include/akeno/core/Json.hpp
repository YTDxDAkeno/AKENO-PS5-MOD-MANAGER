// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded, non-throwing JSON parsing and type-checked field access.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Result.hpp"

namespace akeno::json {

using Json = nlohmann::json;

// Parses `text` only if it is at most `maxBytes` long and nests at most `maxDepth` levels.
// Never throws. Returns ParseError on malformed input, ResponseTooLarge on size violations.
Result<Json> parseBounded(std::string_view text, std::size_t maxBytes,
                          int maxDepth = limits::kMaxJsonDepth);

// Field accessors. They return std::nullopt when the key is missing or has the wrong type.
std::optional<std::string> getString(const Json& object, std::string_view key);
std::optional<std::int64_t> getInt(const Json& object, std::string_view key);
std::optional<bool> getBool(const Json& object, std::string_view key);
const Json* getArray(const Json& object, std::string_view key);
const Json* getObject(const Json& object, std::string_view key);

// String field cleaned for display (valid UTF-8, no control characters, bounded length).
std::string displayString(const Json& object, std::string_view key, std::string_view fallback = {},
                          std::size_t maxBytes = limits::kMaxDisplayStringBytes);

}  // namespace akeno::json
