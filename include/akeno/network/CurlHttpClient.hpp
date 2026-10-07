// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "akeno/network/Http.hpp"

namespace akeno::network {

// Owns curl_global_init / curl_global_cleanup. Create exactly one, before any client is used.
class CurlGlobal {
public:
    CurlGlobal();
    ~CurlGlobal();
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

struct CurlClientOptions {
    // PEM bundle of trusted CAs. On PS5 this is the embedded Mozilla bundle, because the
    // console has no CA file libcurl could use. Empty means "use libcurl's default store".
    std::string_view caBundlePem;
    std::string userAgent;
};

// libcurl-based client. Certificate and host-name verification are always on; there is no
// option to disable them.
class CurlHttpClient final : public IHttpClient {
public:
    explicit CurlHttpClient(CurlClientOptions options);
    Result<HttpResponse> send(const HttpRequest& request, const CancellationToken* cancel = nullptr) override;
    Result<HttpResponse> stream(const HttpRequest& request, IBodySink& sink,
                                const CancellationToken* cancel = nullptr) override;

private:
    Result<HttpResponse> perform(const HttpRequest& request, IBodySink* sink, const CancellationToken* cancel);
    CurlClientOptions options_;
};

// Version string of the linked libcurl and its TLS backend, for diagnostics.
std::string curlVersionDescription();

}  // namespace akeno::network
