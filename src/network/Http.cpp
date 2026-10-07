// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/network/Http.hpp"

#include "akeno/core/Strings.hpp"
#include "akeno/network/Url.hpp"

namespace akeno::network {

std::string_view toString(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get: return "GET";
        case HttpMethod::Head: return "HEAD";
        case HttpMethod::Post: return "POST";
    }
    return "GET";
}

std::string HttpResponse::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (strings::equalsIgnoreCaseAscii(key, name)) {
            return value;
        }
    }
    return {};
}

Result<HttpResponse> IHttpClient::stream(const HttpRequest& request, IBodySink& sink, const CancellationToken* cancel) {
    auto response = send(request, cancel);
    if (!response) {
        return response;
    }
    AKENO_TRY(sink.begin(response->status, response->headers));
    if (!response->body.empty()) {
        AKENO_TRY(sink.write(response->body));
    }
    response->body.clear();
    return response;
}

Status checkRequestPolicy(const HttpRequest& request) {
    auto url = parseUrl(request.url);
    if (!url) {
        return std::move(url).error();
    }
    if (!url->isHttps() && !isLoopbackHost(url->host)) {
        return makeError(ErrorCode::SafetyViolation,
                         "Unencrypted connections are only allowed to this console itself.",
                         url->toString());
    }
    for (const auto& [name, value] : request.headers) {
        auto hasLineBreak = [](std::string_view text) {
            return text.find('\r') != std::string_view::npos || text.find('\n') != std::string_view::npos;
        };
        if (name.empty() || hasLineBreak(name) || hasLineBreak(value) || name.find(':') != std::string::npos) {
            return makeError(ErrorCode::InvalidArgument, "A request header is malformed.", name);
        }
    }
    if (request.maxResponseBytes == 0 || request.connectTimeoutMs <= 0 || request.totalTimeoutMs <= 0 ||
        request.maxRedirects < 0 || request.maxRedirects > limits::kMaxRedirects) {
        return makeError(ErrorCode::InvalidArgument, "Request limits are not valid.");
    }
    return {};
}

}  // namespace akeno::network
