// SPDX-License-Identifier: GPL-3.0-or-later
// AkenoSelfCheck: the first rung of the hardware testing ladder (docs/safety-model.md §9).
// A separate, minimal payload without SDL or video output, so it can be sent to any ELF loader
// (which usually cannot pass arguments). It runs the system check, lists the games reported by
// ShadowMountPlus and, when the network works, downloads and verifies a small harmless file
// (deleted again). The reports go to /data/akeno-mod-manager/logs/.
#include <cstdio>

#include "akeno/app/AppContext.hpp"
#include "akeno/app/CommandLine.hpp"
#include "akeno/app/Headless.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/platform/Platform.hpp"

int main(int argc, char* argv[]) {
    using namespace akeno;
    app::CommandLine commandLine = app::parseCommandLine(argc, argv);
    if (!commandLine.errors.empty()) {
        for (const auto& error : commandLine.errors) std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }
    auto context = app::AppContext::create(commandLine, platform::createPlatform());
    if (!context) {
        std::fprintf(stderr, "Akeno self-check could not start: %s\n", context.error().describe().c_str());
        platform::createPlatform()->notify("Akeno self-check could not start: " + context.error().message);
        return 1;
    }
    app::SystemReport report;
    int exitCode = app::runSelfCheck(*context.value(), &report);
    if (context.value()->library() != nullptr) {
        // Only reads the list; a failure is reported in the log and the notification.
        (void)app::runListGames(*context.value());
    }
    // Ladder steps 5 and 6, only when the network check passed.
    const app::CheckResult* network = report.find(app::CheckId::Networking);
    if (network != nullptr && network->status == app::CheckStatus::Ok) {
        app::DownloadTestSpec spec = app::builtinDownloadTest();
        spec.hardLinks = report.hardLinksSupported;
        // Ladder step 10 only where installing is possible (ShadowMountPlus and its folder found).
        spec.overlayTest = !report.features.safeMode();
        (void)app::runDownloadTest(*context.value(), spec);
    }
    logging::logger().flush();
    return exitCode;
}
