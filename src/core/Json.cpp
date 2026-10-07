// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Json.hpp"

#include "akeno/core/Strings.hpp"

namespace akeno::json {

Result<Json> parseBounded(std::string_view text, std::size_t maxBytes, int maxDepth) {
    if (text.size() > maxBytes) {
        return makeError(ErrorCode::ResponseTooLarge, "The data received is larger than allowed.",
                         strings::concat("json size ", text.size(), " > limit ", maxBytes));
    }
    bool depthExceeded = false;
    Json::parser_callback_t callback = [&](int depth, Json::parse_event_t /*event*/, Json& /*value*/) {
        if (depth > maxDepth) {
            depthExceeded = true;
            return false;  // discard; parsing continues but the result is rejected below
        }
        return true;
    };
    Json parsed = Json::parse(text.begin(), text.end(), callback, /*allow_exceptions=*/false,
                              /*ignore_comments=*/false);
    if (depthExceeded) {
        return makeError(ErrorCode::ParseError, "The data received is nested too deeply.",
                         strings::concat("json depth > ", maxDepth));
    }
    if (parsed.is_discarded()) {
        return makeError(ErrorCode::ParseError, "The data received is not valid JSON.");
    }
    return parsed;
}

namespace {

const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    auto it = object.find(key);
    if (it == object.end()) {
        return nullptr;
    }
    return &*it;
}

}  // namespace

std::optional<std::string> getString(const Json& object, std::string_view key) {
    const Json* value = find(object, key);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return value->get_ref<const std::string&>();
}

std::optional<std::int64_t> getInt(const Json& object, std::string_view key) {
    const Json* value = find(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    // is_number_integer() is also true for unsigned values, so check unsigned first: values above
    // INT64_MAX must be rejected, not wrapped into negative numbers.
    if (value->is_number_unsigned()) {
        auto unsignedValue = value->get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(INT64_MAX)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(unsignedValue);
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>();
    }
    return std::nullopt;
}

std::optional<bool> getBool(const Json& object, std::string_view key) {
    const Json* value = find(object, key);
    if (value == nullptr || !value->is_boolean()) {
        return std::nullopt;
    }
    return value->get<bool>();
}

const Json* getArray(const Json& object, std::string_view key) {
    const Json* value = find(object, key);
    return (value != nullptr && value->is_array()) ? value : nullptr;
}

const Json* getObject(const Json& object, std::string_view key) {
    const Json* value = find(object, key);
    return (value != nullptr && value->is_object()) ? value : nullptr;
}

std::string displayString(const Json& object, std::string_view key, std::string_view fallback,
                          std::size_t maxBytes) {
    auto value = getString(object, key);
    if (!value || value->empty()) {
        return strings::sanitizeForDisplay(fallback, maxBytes);
    }
    return strings::sanitizeForDisplay(*value, maxBytes);
}

}  // namespace akeno::json
