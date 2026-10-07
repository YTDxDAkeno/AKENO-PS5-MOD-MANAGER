// SPDX-License-Identifier: GPL-3.0-or-later
#include "LocalHttpServer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace akeno::test {

namespace {

bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

}  // namespace

LocalHttpServer::LocalHttpServer(Handler handler) : handler_(std::move(handler)) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return;
    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listenFd_, 16) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return;
    }
    socklen_t length = sizeof(addr);
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &length);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
}

LocalHttpServer::~LocalHttpServer() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    if (listenFd_ >= 0) ::close(listenFd_);
}

std::vector<ReceivedRequest> LocalHttpServer::received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
}

void LocalHttpServer::serve() {
    while (!stopping_) {
        pollfd pfd{listenFd_, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) continue;
        handleConnection(fd);
        ::close(fd);
    }
}

void LocalHttpServer::handleConnection(int fd) {
    std::string data;
    char buffer[4096];
    std::size_t headerEnd = std::string::npos;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (headerEnd == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<std::size_t>(n));
        headerEnd = data.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) return;

    ReceivedRequest request;
    std::string head = data.substr(0, headerEnd);
    std::size_t lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd);
    std::size_t sp1 = requestLine.find(' ');
    std::size_t sp2 = requestLine.find(' ', sp1 + 1);
    request.method = requestLine.substr(0, sp1);
    request.target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    std::size_t contentLength = 0;
    std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        std::string line = head.substr(pos, next - pos);
        std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            if (lower(name) == "content-length") contentLength = std::stoul(value);
            request.headers.emplace_back(name, value);
        }
        pos = next + 2;
    }
    request.body = data.substr(headerEnd + 4);
    while (request.body.size() < contentLength && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.body.append(buffer, static_cast<std::size_t>(n));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        received_.push_back(request);
    }
    CannedResponse response = handler_(request);
    if (response.delayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(response.delayMs));
    }
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " Status\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    if (!response.contentLengthOverride.empty()) {
        out += "Content-Length: " + response.contentLengthOverride + "\r\n";
    } else if (!response.omitContentLength) {
        out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    }
    for (const auto& [name, value] : response.headers) {
        out += name + ": " + value + "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    if (!sendAll(fd, out) || request.method == "HEAD") {
        return;
    }
    const std::string body = response.body.substr(0, std::min(response.body.size(), response.closeAfterBytes));
    const std::size_t chunk = response.chunkSize > 0 ? response.chunkSize : body.size();
    for (std::size_t offset = 0; offset < body.size() && !stopping_; offset += chunk) {
        if (!sendAll(fd, body.substr(offset, chunk))) return;
        if (response.chunkDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(response.chunkDelayMs));
    }
}

}  // namespace akeno::test
