// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/network/CurlHttpClient.hpp"

#include <charconv>
#include <memory>
#include <optional>

#include <curl/curl.h>

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/network/Url.hpp"

namespace akeno::network {

CurlGlobal::CurlGlobal() { ok_ = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }

CurlGlobal::~CurlGlobal() {
    if (ok_) {
        curl_global_cleanup();
    }
}

std::string curlVersionDescription() {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr) {
        return "libcurl (unknown version)";
    }
    std::string text = std::string("libcurl ") + info->version;
    if (info->ssl_version != nullptr) {
        text += std::string(" / ") + info->ssl_version;
    }
    return text;
}

namespace {

struct TransferState {
    std::size_t maxBytes = 0;
    bool isHead = false;
    bool tooLarge = false;
    std::string body;
    HeaderList headers;
    const CancellationToken* cancel = nullptr;
    // Streaming
    CURL* handle = nullptr;
    IBodySink* sink = nullptr;
    bool begun = false;
    std::uint64_t received = 0;
    std::optional<Error> sinkError;
};

bool beginSink(TransferState& state) {
    if (state.begun) return true;
    state.begun = true;
    long status = 0;
    curl_easy_getinfo(state.handle, CURLINFO_RESPONSE_CODE, &status);
    auto started = state.sink->begin(status, state.headers);
    if (!started) {
        state.sinkError = started.error();
        return false;
    }
    return true;
}

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* userData) {
    auto* state = static_cast<TransferState*>(userData);
    const std::size_t bytes = size * count;
    if (state->received + bytes > state->maxBytes) {
        state->tooLarge = true;
        return 0;  // abort the transfer
    }
    state->received += bytes;
    if (state->sink == nullptr) {
        state->body.append(data, bytes);
        return bytes;
    }
    // Bodies of followed redirects are not delivered here, so this is the final response.
    if (!beginSink(*state)) return 0;
    auto written = state->sink->write(std::string_view(data, bytes));
    if (!written) {
        state->sinkError = written.error();
        return 0;
    }
    return bytes;
}

std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* userData) {
    auto* state = static_cast<TransferState*>(userData);
    const std::size_t bytes = size * count;
    std::string_view line(data, bytes);
    if (strings::startsWith(line, "HTTP/")) {
        state->headers.clear();  // a new response begins (redirect or final)
        return bytes;
    }
    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return bytes;
    }
    std::string name(strings::trim(line.substr(0, colon)));
    std::string value(strings::trim(line.substr(colon + 1)));
    // Buffered requests stop early on an oversized Content-Length. Streams leave the check to the
    // sink, which compares it with the exact size it expects (the body limit still applies).
    if (!state->isHead && state->sink == nullptr && strings::equalsIgnoreCaseAscii(name, "content-length")) {
        std::uint64_t length = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
        if (ec == std::errc() && ptr == value.data() + value.size() && length > state->maxBytes) {
            state->tooLarge = true;
            return 0;
        }
    }
    if (state->headers.size() < 128) {
        state->headers.emplace_back(std::move(name), std::move(value));
    }
    return bytes;
}

int onProgress(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<TransferState*>(userData);
    return (state->cancel != nullptr && state->cancel->cancelled()) ? 1 : 0;
}

Error mapCurlError(CURLcode code, const TransferState& state, const Url& url, const char* errorBuffer) {
    std::string detail = strings::concat(curl_easy_strerror(code), " (", static_cast<int>(code), ")");
    if (errorBuffer != nullptr && errorBuffer[0] != '\0') {
        detail += strings::concat(": ", errorBuffer);
    }
    const std::string host = url.authority();
    if (state.tooLarge) {
        return makeError(ErrorCode::ResponseTooLarge,
                         "The server sent more data than allowed. The transfer was stopped.", detail);
    }
    switch (code) {
        case CURLE_ABORTED_BY_CALLBACK:
            return makeError(ErrorCode::Cancelled, "The transfer was cancelled.", detail);
        case CURLE_OPERATION_TIMEDOUT:
            return makeError(ErrorCode::Timeout, "The connection to " + host + " timed out.", detail);
        case CURLE_COULDNT_RESOLVE_HOST:
            return makeError(ErrorCode::Network, "Could not find " + host + ". Check the internet connection.",
                             detail);
        case CURLE_COULDNT_CONNECT:
            return makeError(ErrorCode::Unavailable, "Could not connect to " + host + ".", detail);
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
            return makeError(ErrorCode::TlsError,
                             "The secure connection to " + host + " could not be verified. Nothing was downloaded.",
                             detail);
        case CURLE_UNSUPPORTED_PROTOCOL:
            return makeError(ErrorCode::SafetyViolation,
                             "The server redirected to an address that is not allowed.", detail);
        case CURLE_PARTIAL_FILE:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
            return makeError(ErrorCode::Network, "The connection to " + host + " ended before the transfer was complete.",
                             detail);
        case CURLE_TOO_MANY_REDIRECTS:
            return makeError(ErrorCode::Network, "The server redirected too many times.", detail);
        default:
            return makeError(ErrorCode::Network, "The request to " + host + " failed.", detail);
    }
}

struct EasyDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};
struct SlistDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

}  // namespace

CurlHttpClient::CurlHttpClient(CurlClientOptions options) : options_(std::move(options)) {
    if (options_.userAgent.empty()) {
        options_.userAgent = "AkenoModManager";
    }
}

Result<HttpResponse> CurlHttpClient::send(const HttpRequest& request, const CancellationToken* cancel) {
    return perform(request, nullptr, cancel);
}

Result<HttpResponse> CurlHttpClient::stream(const HttpRequest& request, IBodySink& sink, const CancellationToken* cancel) {
    return perform(request, &sink, cancel);
}

Result<HttpResponse> CurlHttpClient::perform(const HttpRequest& request, IBodySink* sink, const CancellationToken* cancel) {
    AKENO_TRY(checkRequestPolicy(request));
    auto url = parseUrl(request.url);
    if (!url) {
        return std::move(url).error();
    }
    std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
    if (!easy) {
        return makeError(ErrorCode::Internal, "Could not start a network request.", "curl_easy_init failed");
    }
    CURL* handle = easy.get();

    TransferState state;
    state.maxBytes = request.maxResponseBytes;
    state.isHead = request.method == HttpMethod::Head;
    state.cancel = cancel;
    state.handle = handle;
    state.sink = sink;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    const std::string urlText = url->toString();

    curl_easy_setopt(handle, CURLOPT_URL, urlText.c_str());
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    // Loopback traffic (the ShadowMountPlus API) must never be sent through a proxy.
    curl_easy_setopt(handle, CURLOPT_NOPROXY, "localhost,127.0.0.0/8,::1");
    curl_easy_setopt(handle, CURLOPT_USERAGENT, options_.userAgent.c_str());
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, request.connectTimeoutMs);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, request.totalTimeoutMs);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 15L);
    if (request.acceptCompressed) {
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");  // decompressed size still counts against the limit
    }

    // TLS verification is mandatory.
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_blob caBlob{};
    if (!options_.caBundlePem.empty()) {
        caBlob.data = const_cast<char*>(options_.caBundlePem.data());
        caBlob.len = options_.caBundlePem.size();
        caBlob.flags = CURL_BLOB_NOCOPY;
        curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &caBlob);
    }

    // Redirects: only for HTTPS, and only to HTTPS.
    if (url->isHttps() && request.maxRedirects > 0) {
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, static_cast<long>(request.maxRedirects));
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    } else {
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    }

    std::unique_ptr<curl_slist, SlistDeleter> headerList;
    for (const auto& [name, value] : request.headers) {
        std::string line = name + ": " + value;
        curl_slist* appended = curl_slist_append(headerList.get(), line.c_str());
        if (appended == nullptr) {
            return makeError(ErrorCode::Internal, "Could not prepare request headers.");
        }
        headerList.release();
        headerList.reset(appended);
    }
    if (headerList) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList.get());
    }

    switch (request.method) {
        case HttpMethod::Get:
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
            break;
        case HttpMethod::Head:
            curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
            break;
        case HttpMethod::Post:
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            break;
    }

    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &onBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &onHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &onProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);

    CURLcode code = curl_easy_perform(handle);
    if (state.sinkError) {
        return *state.sinkError;  // the receiver stopped the transfer; its reason wins
    }
    if (code != CURLE_OK) {
        return mapCurlError(code, state, url.value(), errorBuffer);
    }
    if (sink != nullptr && !beginSink(state)) {
        return *state.sinkError;  // a response without a body
    }

    HttpResponse response;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    char* effective = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective != nullptr) {
        response.effectiveUrl = effective;
    }
    response.body = std::move(state.body);
    response.headers = std::move(state.headers);
    return response;
}

}  // namespace akeno::network
