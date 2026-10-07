// SPDX-License-Identifier: GPL-3.0-or-later
// Akeno PS5 Mod Manager — Result/Error types used across all modules.
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace akeno {

enum class ErrorCode {
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    PermissionDenied,
    NoSpace,
    Network,
    Timeout,
    TlsError,
    HttpStatus,
    ResponseTooLarge,
    ParseError,
    SchemaError,
    Unsupported,
    Busy,
    Unavailable,
    SafetyViolation,
    Database,
    Cancelled,
    Internal,
};

std::string_view toString(ErrorCode code) noexcept;

// `message` is written for the user ("The ShadowMount API did not answer.").
// `detail` carries the technical cause for the log ("connect(): ECONNREFUSED").
struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
    std::string detail;

    std::string describe() const;
};

inline Error makeError(ErrorCode code, std::string message, std::string detail = {}) {
    return Error{code, std::move(message), std::move(detail)};
}

template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> is not meaningful");

public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & {
        assert(ok());
        return std::get<0>(storage_);
    }
    const T& value() const& {
        assert(ok());
        return std::get<0>(storage_);
    }
    T&& value() && {
        assert(ok());
        return std::get<0>(std::move(storage_));
    }

    const Error& error() const& {
        assert(!ok());
        return std::get<1>(storage_);
    }
    Error&& error() && {
        assert(!ok());
        return std::get<1>(std::move(storage_));
    }

    T valueOr(T fallback) const& { return ok() ? std::get<0>(storage_) : std::move(fallback); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    static Result success() { return Result(); }

    bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const& {
        assert(!ok());
        return *error_;
    }
    Error&& error() && {
        assert(!ok());
        return std::move(*error_);
    }

private:
    std::optional<Error> error_;
};

using Status = Result<void>;

}  // namespace akeno

// Propagate an error from an expression returning Result<...>.
#define AKENO_TRY(expr)                                   \
    do {                                                  \
        auto akenoTryResult_ = (expr);                    \
        if (!akenoTryResult_.ok()) {                      \
            return std::move(akenoTryResult_).error();    \
        }                                                 \
    } while (false)
