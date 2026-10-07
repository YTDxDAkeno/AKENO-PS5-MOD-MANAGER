// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers for the host test suite.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "akeno/games/IGameDiscoveryProvider.hpp"
#include "akeno/network/Http.hpp"
#include "akeno/platform/Platform.hpp"

namespace akeno::test {

// Creates a unique directory under the system temp dir and removes it on destruction.
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "akeno-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        path_ = created != nullptr ? std::filesystem::path(created) : std::filesystem::path();
        // Resolve symlinks (e.g. macOS /tmp) so paths compare equal to what WriteGuard resolves.
        std::error_code ec;
        auto canonical = std::filesystem::canonical(path_, ec);
        if (!ec) path_ = canonical;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::string fixturePath(const std::string& relative) {
    return std::string(AKENO_TEST_FIXTURES) + "/" + relative;
}

inline std::string readFixture(const std::string& relative) {
    std::ifstream in(fixturePath(relative), std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// Records requests and answers from a table keyed by "METHOD URL".
class MockHttpClient final : public network::IHttpClient {
public:
    struct Entry {
        Result<network::HttpResponse> response;
    };

    void respond(network::HttpMethod method, const std::string& url, long status, std::string body) {
        network::HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        response.effectiveUrl = url;
        std::lock_guard<std::mutex> lock(mutex_);
        table_.insert_or_assign(key(method, url), Result<network::HttpResponse>(std::move(response)));
    }

    void fail(network::HttpMethod method, const std::string& url, Error error) {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.insert_or_assign(key(method, url), Result<network::HttpResponse>(std::move(error)));
    }

    Result<network::HttpResponse> send(const network::HttpRequest& request,
                                       const CancellationToken* /*cancel*/) override {
        auto policy = network::checkRequestPolicy(request);
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        if (!policy) {
            return std::move(policy).error();
        }
        auto it = table_.find(key(request.method, request.url));
        if (it == table_.end()) {
            return makeError(ErrorCode::Unavailable, "mock: no route", request.url);
        }
        if (it->second.ok() && it->second.value().body.size() > request.maxResponseBytes) {
            return makeError(ErrorCode::ResponseTooLarge, "mock: too large");
        }
        return it->second;
    }

    std::vector<network::HttpRequest> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    static std::string key(network::HttpMethod method, const std::string& url) {
        return std::string(network::toString(method)) + " " + url;
    }

    mutable std::mutex mutex_;
    std::map<std::string, Result<network::HttpResponse>> table_;
    std::vector<network::HttpRequest> requests_;
};

class FakePlatform final : public platform::IPlatform {
public:
    bool console = false;
    platform::FirmwareInfo firmwareInfo{false, 0, "not applicable (test)"};
    std::vector<std::string> notifications;

    std::string_view name() const override { return "Test"; }
    bool isConsole() const override { return console; }
    platform::FirmwareInfo firmware() const override { return firmwareInfo; }
    platform::HomebrewEnvironment probeHomebrewEnvironment(const std::filesystem::path&) const override {
        platform::HomebrewEnvironment env;
        env.isConsole = console;
        env.dataWritable = true;
        env.summary = console ? "detected - /data is writable" : "test environment";
        return env;
    }
    void notify(std::string_view message) override { notifications.emplace_back(message); }
    std::filesystem::path defaultDataRoot() const override { return "/nonexistent-default"; }
    void exitApplicationContext() override {}
};

class FakeGameProvider final : public games::IGameDiscoveryProvider {
public:
    Result<games::DiscoveryStatus> status = games::DiscoveryStatus{true, "Fake", "1.0", "connected", true};
    Result<std::vector<games::GameInfo>> games = std::vector<games::GameInfo>{};
    std::map<std::string, std::string> icons;
    int discoverCalls = 0;

    std::string name() const override { return "Fake"; }
    Result<games::DiscoveryStatus> probe() override { return status; }
    Result<std::vector<games::GameInfo>> discoverGames() override {
        ++discoverCalls;
        return games;
    }
    Result<std::string> loadIcon(const games::GameInfo& game, games::IconSize) override {
        auto it = icons.find(game.titleId);
        if (it == icons.end()) return makeError(ErrorCode::NotFound, "no icon");
        return it->second;
    }
};

inline games::GameInfo makeGame(std::string titleId, std::string name, std::string version,
                                games::Platform platform = games::Platform::Ps5) {
    games::GameInfo game;
    game.titleId = std::move(titleId);
    game.name = std::move(name);
    game.version = std::move(version);
    game.platform = platform;
    game.sourceType = games::SourceType::Folder;
    return game;
}

}  // namespace akeno::test
