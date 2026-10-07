// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 implementation (ps5-payload-sdk). Behaviour on hardware is unverified; see
// docs/compatibility.md for the recorded test status.
#include <cstring>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "akeno/core/AppPaths.hpp"
#include "akeno/platform/Platform.hpp"

extern "C" {
// Plain system notification, as used by ShadowMountPlus and other payloads.
struct AkenoNotifyRequest {
    char unused[45];
    char message[3075];
};
int sceKernelSendNotificationRequest(int device, AkenoNotifyRequest* request, std::size_t size, int blocking);
int sceSystemServiceLoadExec(const char* path, const char* const* argv);
}

namespace akeno::platform {

namespace {

bool isDirectory(const char* path) {
    struct stat info {};
    return ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

class Ps5Platform final : public IPlatform {
public:
    std::string_view name() const override { return "PS5"; }
    bool isConsole() const override { return true; }

    FirmwareInfo firmware() const override {
        FirmwareInfo info;
        info.raw = kernel_get_fw_version();
        auto formatted = formatFirmwareVersion(info.raw);
        info.known = formatted.has_value();
        info.display = formatted.value_or("unknown");
        return info;
    }

    HomebrewEnvironment probeHomebrewEnvironment(const std::filesystem::path& /*dataRoot*/) const override {
        HomebrewEnvironment env;
        env.isConsole = true;
        env.dataWritable = ::access("/data", W_OK) == 0;
        struct Probe {
            const char* path;
            const char* label;
        };
        static constexpr Probe kProbes[] = {
            {"/data/shadowmount", "ShadowMountPlus data (/data/shadowmount)"},
            {"/data/etaHEN", "etaHEN data (/data/etaHEN)"},
            {"/data/homebrew", "homebrew directory (/data/homebrew)"},
        };
        for (const auto& probe : kProbes) {
            if (isDirectory(probe.path)) {
                env.components.emplace_back(probe.label);
            }
        }
        env.summary = env.dataWritable ? "detected — /data is writable" : "/data is NOT writable";
        if (::getuid() == 0) {
            env.summary += ", running with root credentials";
        }
        return env;
    }

    void notify(std::string_view message) override {
        AkenoNotifyRequest request{};
        std::size_t length = message.size() < sizeof(request.message) - 1 ? message.size()
                                                                           : sizeof(request.message) - 1;
        std::memcpy(request.message, message.data(), length);
        request.message[length] = '\0';
        (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    }

    std::filesystem::path defaultDataRoot() const override { return std::string(kPs5DefaultDataRoot); }

    void exitApplicationContext() override {
        // Mirrors the SDL PS5 port's own main(): closes the app context created by the
        // homebrew launcher. Only called at the end of an interactive UI session.
        (void)sceSystemServiceLoadExec("exit", nullptr);
    }
};

}  // namespace

std::unique_ptr<IPlatform> createPlatform() { return std::make_unique<Ps5Platform>(); }

}  // namespace akeno::platform
