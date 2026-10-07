// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal HTTP/1.1 server on 127.0.0.1 for exercising the real libcurl client in tests.
// One request per connection, like the ShadowMountPlus API.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace akeno::test {

struct ReceivedRequest {
    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct CannedResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    bool omitContentLength = false;   // stream until close
    int delayMs = 0;                  // wait before answering
    std::vector<std::pair<std::string, std::string>> headers;  // extra headers
    std::size_t chunkSize = 0;        // > 0: send the body in pieces ...
    int chunkDelayMs = 0;             // ... with this pause between them
    std::size_t closeAfterBytes = static_cast<std::size_t>(-1);  // drop the connection early
    std::string contentLengthOverride; // announce a different Content-Length
};

class LocalHttpServer {
public:
    using Handler = std::function<CannedResponse(const ReceivedRequest&)>;

    explicit LocalHttpServer(Handler handler);
    ~LocalHttpServer();
    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    std::uint16_t port() const { return port_; }
    bool ok() const { return listenFd_ >= 0; }
    std::vector<ReceivedRequest> received() const;

private:
    void serve();
    void handleConnection(int fd);

    Handler handler_;
    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<ReceivedRequest> received_;
};

}  // namespace akeno::test
