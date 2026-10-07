// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/app/Headless.hpp"

#include <cstdio>

#include "akeno/core/Strings.hpp"

namespace akeno::app {

using logging::logger;

int runSelfCheck(AppContext& context) {
    SystemChecker checker = context.makeSystemChecker();
    SystemReport report = checker.run();
    std::string text = report.toText();
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);

    auto file = context.writeReport("system-check", text);
    if (file) {
        logger().info("syscheck", "report written: " + file->string());
    } else {
        logger().error("syscheck", "could not write the report: " + file.error().describe());
    }
    const CheckResult* api = report.find(CheckId::ShadowMountApi);
    std::string notification =
        strings::concat("Akeno self-check done. ShadowMount: ",
                        (api != nullptr && api->status == CheckStatus::Ok) ? "connected" : "not connected",
                        ". Safe mode: ", report.features.safeMode() ? "ON" : "OFF", ".");
    context.platform().notify(notification);
    return 0;
}

int runListGames(AppContext& context) {
    games::GameLibrary* library = context.library();
    if (library == nullptr) {
        std::fprintf(stdout, "ShadowMountPlus is not configured.\n");
        context.platform().notify("Akeno: ShadowMountPlus is not configured.");
        return 1;
    }
    auto snapshot = library->refresh();
    if (!snapshot) {
        std::string message = "Could not read the game list: " + snapshot.error().message;
        std::fprintf(stdout, "%s\n", message.c_str());
        logger().error("games", snapshot.error().describe());
        context.platform().notify("Akeno: " + snapshot.error().message);
        return 1;
    }
    std::string text = strings::concat("Installed games reported by ShadowMountPlus (", snapshot->games.size(),
                                       ")\n\n");
    for (const auto& game : snapshot->games) {
        text += strings::concat(game.titleId, "  ", game.name, "\n");
        text += strings::concat("    version ", game.displayVersion(), ", ", games::toString(game.platform), ", ",
                                games::displayName(game.sourceType), game.mounted ? ", mounted" : "", "\n");
        if (!game.installPath.empty()) text += "    path    " + game.installPath + "\n";
        if (!game.runtimePath.empty()) text += "    runtime " + game.runtimePath + "\n";
        if (game.previousVersion) text += "    previously seen version " + *game.previousVersion + "\n";
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    auto file = context.writeReport("games", text);
    if (!file) {
        logger().error("games", "could not write the report: " + file.error().describe());
    }
    context.platform().notify(strings::concat("Akeno: ", snapshot->games.size(), " games found."));
    return 0;
}

}  // namespace akeno::app
