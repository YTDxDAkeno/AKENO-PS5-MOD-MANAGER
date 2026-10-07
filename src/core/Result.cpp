// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Result.hpp"

namespace akeno {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidArgument: return "invalid-argument";
        case ErrorCode::NotFound: return "not-found";
        case ErrorCode::AlreadyExists: return "already-exists";
        case ErrorCode::IoError: return "io-error";
        case ErrorCode::PermissionDenied: return "permission-denied";
        case ErrorCode::NoSpace: return "no-space";
        case ErrorCode::Network: return "network";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::TlsError: return "tls-error";
        case ErrorCode::HttpStatus: return "http-status";
        case ErrorCode::ResponseTooLarge: return "response-too-large";
        case ErrorCode::ParseError: return "parse-error";
        case ErrorCode::SchemaError: return "schema-error";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::Busy: return "busy";
        case ErrorCode::Unavailable: return "unavailable";
        case ErrorCode::SafetyViolation: return "safety-violation";
        case ErrorCode::Database: return "database";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::Internal: return "internal";
    }
    return "unknown";
}

std::string Error::describe() const {
    std::string text;
    text.reserve(message.size() + detail.size() + 32);
    text.append("[").append(toString(code)).append("] ").append(message);
    if (!detail.empty()) {
        text.append(" (").append(detail).append(")");
    }
    return text;
}

}  // namespace akeno
