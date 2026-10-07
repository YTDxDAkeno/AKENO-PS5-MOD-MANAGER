// SPDX-License-Identifier: GPL-3.0-or-later
// Host (desktop) implementation used for development and tests. It never pretends to be a
// console: firmware is reported as not applicable and no homebrew components are claimed.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

#include <unistd.h>

#include "akeno/platform/Platform.hpp"

namespace akeno::platform {

namespace {

class HostPlatform final : public IPlatform {
public:
    std::string_view name() const override {
#if defined(__APPLE__)
        return "Host (macOS)";
#else
        return "Host (Linux)";
#endif
    }

    bool isConsole() const override { return false; }

    FirmwareInfo firmware() const override {
        FirmwareInfo info;
        info.known = false;
        info.display = "not applicable (host build)";
        return info;
    }

    HomebrewEnvironment probeHomebrewEnvironment(const std::filesystem::path& dataRoot) const override {
        HomebrewEnvironment env;
        env.isConsole = false;
        auto parent = dataRoot.parent_path();
        env.dataWritable = ::access(parent.c_str(), W_OK) == 0;
        env.summary = env.dataWritable ? "host build — data directory writable"
                                       : "host build — data directory NOT writable";
        return env;
    }

    void notify(std::string_view message) override {
        std::fprintf(stderr, "[notification] %.*s\n", static_cast<int>(message.size()), message.data());
    }

    std::filesystem::path defaultDataRoot() const override {
        if (const char* env = std::getenv("AKENO_DATA_ROOT"); env != nullptr && env[0] == '/') {
            return env;
        }
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (ec) {
            return "/tmp/akeno-data";
        }
        return cwd / "akeno-data";
    }

    void exitApplicationContext() override {}
};

}  // namespace

std::unique_ptr<IPlatform> createPlatform() { return std::make_unique<HostPlatform>(); }

}  // namespace akeno::platform
