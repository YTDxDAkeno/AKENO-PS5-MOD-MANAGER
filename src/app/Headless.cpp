// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/app/Headless.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

#include "akeno/core/Strings.hpp"
#include "akeno/downloads/DownloadManager.hpp"

namespace akeno::app {

using logging::logger;

int runSelfCheck(AppContext& context, SystemReport* reportOut) {
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
    if (reportOut != nullptr) *reportOut = std::move(report);
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

DownloadTestSpec builtinDownloadTest() {
    return DownloadTestSpec{
        "https://raw.githubusercontent.com/YTDxDAkeno/AKENO-PS5-MOD-MANAGER/main/assets/test/download-test.zip",
        AKENO_DOWNLOAD_TEST_SHA256, AKENO_DOWNLOAD_TEST_SIZE};
}

int runDownloadTest(AppContext& context, const DownloadTestSpec& spec) {
    using namespace std::chrono;
    downloads::DownloadManager& manager = context.downloads();
    std::string text = "Akeno download test\n\n";
    text += strings::concat("File:     ", spec.url, "\n");
    text += strings::concat("Expected: ", spec.size, " bytes, SHA-256 ", spec.sha256, "\n");
    text += strings::concat("Folder:   ", manager.directory().string(), "\n\n");

    bool passed = false;
    auto finish = [&](const std::string& result) {
        text += result + "\n";
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
        auto file = context.writeReport("download-test", text);
        if (!file) logger().error("downloads", "could not write the report: " + file.error().describe());
        context.platform().notify(passed ? "Akeno download test passed." : "Akeno download test failed: " + result);
        return passed ? 0 : 1;
    };

    downloads::DownloadRequest request;
    request.mod = {"akeno-self-test", "download-test"};
    request.displayName = "Akeno download test";
    request.modVersion = "1";
    request.url = spec.url;
    request.expectedSize = spec.size;
    request.expectedSha256 = spec.sha256;
    request.format = mods::ArchiveFormat::Zip;
    auto valid = downloads::validateRequest(request);
    if (!valid) return finish("Result:   not started - " + valid.error().message);
    auto started = manager.start();
    if (!started) return finish("Result:   not started - " + started.error().message);
    auto queued = manager.enqueue(request);
    if (!queued) return finish("Result:   not started - " + queued.error().message);

    const auto begin = steady_clock::now();
    std::optional<downloads::DownloadInfo> info;
    while (steady_clock::now() - begin < minutes(3)) {
        info = manager.find(queued->id);
        if (!info || info->record.state == downloads::DownloadState::Completed ||
            info->record.state == downloads::DownloadState::Failed) {
            break;
        }
        std::this_thread::sleep_for(milliseconds(100));
    }
    const double seconds = duration<double>(steady_clock::now() - begin).count();
    std::string result;
    if (!info) {
        result = "Result:   FAILED - the download disappeared";
    } else if (info->record.state == downloads::DownloadState::Completed) {
        auto file = manager.completedFile(queued->id);
        passed = file.ok();
        result = passed ? strings::concat("Result:   PASSED - received ", info->record.bytesDone,
                                          " bytes, SHA-256 matches (", static_cast<int>(seconds * 1000), " ms)\n",
                                          "Stored:   ", file->string())
                        : "Result:   FAILED - " + file.error().message;
    } else if (info->record.state == downloads::DownloadState::Failed) {
        result = "Result:   FAILED - " + info->record.error;
    } else {
        result = "Result:   FAILED - no result within three minutes";
    }
    auto removed = manager.remove(queued->id);
    manager.stop();
    result += removed ? "\nCleanup:  the test file was deleted" : "\nCleanup:  FAILED - " + removed.error().message;
    if (!removed) passed = false;
    return finish(result);
}

}  // namespace akeno::app
