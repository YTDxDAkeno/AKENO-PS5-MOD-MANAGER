// SPDX-License-Identifier: GPL-3.0-or-later
// Akeno PS5 Mod Manager entry point.
#include <cstdio>

#include "akeno/app/AppContext.hpp"
#include "akeno/app/CommandLine.hpp"
#include "akeno/app/Headless.hpp"
#include "akeno/core/BuildInfo.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/platform/Platform.hpp"
#include "akeno/ui/SdlApplication.hpp"

int main(int argc, char* argv[]) {
    using namespace akeno;
    const app::CommandLine commandLine = app::parseCommandLine(argc, argv);
    if (!commandLine.errors.empty()) {
        for (const auto& error : commandLine.errors) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
        }
        std::fprintf(stderr, "\n%s", app::usageText().c_str());
        return 2;
    }
    if (commandLine.mode == app::RunMode::Help) {
        std::fputs(app::usageText().c_str(), stdout);
        return 0;
    }
    if (commandLine.mode == app::RunMode::Version) {
        std::printf("Akeno PS5 Mod Manager %.*s (%.*s, %.*s)\n", static_cast<int>(build::version().size()),
                    build::version().data(), static_cast<int>(build::target().size()), build::target().data(),
                    static_cast<int>(build::gitRevision().size()), build::gitRevision().data());
        return 0;
    }

    auto context = app::AppContext::create(commandLine, platform::createPlatform());
    if (!context) {
        const Error& error = context.error();
        std::fprintf(stderr, "Akeno Mod Manager could not start: %s\n", error.describe().c_str());
        platform::createPlatform()->notify("Akeno Mod Manager could not start: " + error.message);
        return 1;
    }

    int exitCode = 0;
    switch (commandLine.mode) {
        case app::RunMode::SelfCheck:
            exitCode = app::runSelfCheck(*context.value());
            break;
        case app::RunMode::ListGames:
            exitCode = app::runListGames(*context.value());
            break;
        case app::RunMode::Interactive:
            exitCode = ui::runSdlApplication(*context.value(), commandLine);
            break;
        case app::RunMode::Help:
        case app::RunMode::Version:
            break;
    }
    logging::logger().info("startup", "exit code " + std::to_string(exitCode));
    logging::logger().flush();
    if (commandLine.mode == app::RunMode::Interactive) {
        // Closes the application context the homebrew launcher created (console only).
        context.value()->platform().exitApplicationContext();
    }
    return exitCode;
}
