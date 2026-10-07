// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"

namespace akeno::network {

enum class HttpMethod { Get, Head, Post };

std::string_view toString(HttpMethod method) noexcept;

using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    HeaderList headers;
    std::string body;
    long connectTimeoutMs = limits::kDefaultConnectTimeoutMs;
    long totalTimeoutMs = limits::kDefaultTotalTimeoutMs;
    std::size_t maxResponseBytes = limits::kMaxJsonSmallResponse;
    int maxRedirects = limits::kMaxRedirects;
    // gzip/deflate transfer encoding. Downloads turn it off: byte ranges must refer to the file
    // itself, and the received size must be the file size.
    bool acceptCompressed = true;
};

struct HttpResponse {
    long status = 0;
    HeaderList headers;     // headers of the final response (after redirects)
    std::string body;
    std::string effectiveUrl;

    // Case-insensitive lookup; empty if absent.
    std::string header(std::string_view name) const;
    bool isSuccess() const noexcept { return status >= 200 && status < 300; }
};

// Receives a streamed response body (downloads), so large files never sit in memory.
class IBodySink {
public:
    virtual ~IBodySink() = default;
    // Called once with the final response's status and headers, before any body bytes and also
    // for responses without a body. Returning an error stops the transfer with that error.
    virtual Status begin(long status, const HeaderList& headers) = 0;
    virtual Status write(std::string_view chunk) = 0;
};

// Transport-level errors only (DNS, TLS, timeouts, size limits, policy). HTTP error statuses are
// returned as responses so callers can explain them.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual Result<HttpResponse> send(const HttpRequest& request, const CancellationToken* cancel = nullptr) = 0;

    // Like send(), but hands the body to `sink` as it arrives. maxResponseBytes still limits the
    // body. The returned response has an empty body. The default implementation buffers through
    // send() (test doubles); the real client streams.
    virtual Result<HttpResponse> stream(const HttpRequest& request, IBodySink& sink,
                                        const CancellationToken* cancel = nullptr);
};

// Network policy (docs/safety-model.md §5): https to anywhere, plain http only to loopback,
// no CR/LF in headers, bounded limits.
Status checkRequestPolicy(const HttpRequest& request);

}  // namespace akeno::network
