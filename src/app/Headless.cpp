// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/app/Headless.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "akeno/core/Strings.hpp"
#include "akeno/downloads/DownloadManager.hpp"
#include "akeno/install/OverlayManager.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/security/Sha256.hpp"

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
        if (!game.versionSource.empty()) text += "    version from " + game.versionSource + "\n";
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

namespace {

// Ladder steps 7 to 9: unpack the test archive into staging, delete staging, describe the plan.
std::string checkTestArchive(AppContext& context, const std::string& downloadId, const std::filesystem::path& archive,
                             std::optional<bool> hardLinks, bool& passed) {
    mods::ModCheckRequest request;
    request.downloadId = downloadId;
    request.archive = archive;
    request.format = mods::ArchiveFormat::Zip;
    request.mod = {"akeno-self-test", "download-test"};
    request.displayName = "Akeno download test";
    request.modVersion = "1";
    request.titleId = "TEST00000";
    // Akeno's own harmless test archive: its layout is defined like a curated manifest (archive
    // root to game root), so the plan can be described without a game.
    request.curated = true;
    mods::ModCheckEnvironment env{context.fs(), context.paths(), context.journal(),
                                  context.interruptedOperation().has_value(), limits::kStorageSafetyReserveBytes, {}, {}};
    auto report = mods::runModCheck(request, env);
    if (!report) {
        passed = false;
        return "Check:    FAILED - " + report.error().message;
    }
    // What is left in staging, by name, so a hardware report shows the cause.
    std::error_code ec;
    std::vector<std::string> leftovers;
    for (std::filesystem::directory_iterator it(context.paths().staging(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (leftovers.size() < 10) leftovers.push_back(it->path().filename().string());
    }
    const bool stagingEmpty = leftovers.empty() && !ec;
    mods::completeReport(report.value(), context.paths(), hardLinks);
    const auto& a = report->analysis;
    std::string text = strings::concat("Check:    PASSED - ", report->archiveFiles, " file(s) unpacked into ",
                                       context.paths().staging().string(), "\n");
    if (stagingEmpty) {
        text += "Staging:  deleted again\n";
    } else if (ec) {
        text += "Staging:  NOT CHECKED - could not list it: " + ec.message() + "\n";
    } else {
        std::string names;
        for (const auto& name : leftovers) names += (names.empty() ? "" : ", ") + name;
        text += "Staging:  NOT EMPTY - left: " + names + "\n";
    }
    text += strings::concat("Analysis: ", a.installCount, " file(s) to install, ", a.findings.size(), " finding(s)\n");
    for (const auto& file : a.files) {
        text += strings::concat("          ", file.archivePath, " (", mods::toString(file.kind), ", ",
                                file.size, " bytes, SHA-256 ", file.sha256.substr(0, 16), "...)\n");
    }
    if (report->plan) {
        text += strings::concat("Plan:     ", report->plan->steps.size(), " steps (described, not carried out here)\n");
        for (const auto& [from, to] : report->plan->mapping) text += "          " + from + " -> " + to + "\n";
        text += strings::concat("Overlay:  ", hardLinks == true    ? "hard links"
                                              : hardLinks == false ? "copies (hard links do not work here)"
                                                                   : "not probed",
                                ", ", strings::formatBytes(report->plan->overlayExtraBytes), " extra\n");
    }
    if (!stagingEmpty) passed = false;
    return text;
}

// Ladder step 10: a harmless overlay for the test title TEST00000 (never a real game), made with
// the real install code and removed again with Vanilla.
std::string overlayTest(AppContext& context, const std::string& downloadId, const std::filesystem::path& archive,
                        bool& passed) {
    constexpr const char* kTitle = "TEST00000";
    install::InstallEnvironment env{context.fs(),
                                    context.paths(),
                                    context.journal(),
                                    context.interruptedOperation().has_value(),
                                    std::filesystem::path(std::string(install::kDefaultBackportsRoot)),
                                    limits::kStorageSafetyReserveBytes,
                                    {}};
    const install::TitleTarget target{kTitle, false, false, {}};
    std::string text = "Overlay test (ladder step 10, title TEST00000 only)\n";
    auto failed = [&](const std::string& step, const std::string& why) {
        passed = false;
        text += strings::concat(step, "FAILED - ", why, "\n");
        // Leave nothing behind: Vanilla, then delete the stored copy.
        (void)install::setVanilla(target, env);
        (void)install::removeStoredMod(env, kTitle, downloadId);
        return text;
    };

    install::InstallRequest request;
    request.downloadId = downloadId;
    request.archive = archive;
    request.format = mods::ArchiveFormat::Zip;
    request.mod = {"akeno-self-test", "download-test"};
    request.name = "Akeno download test";
    request.version = "1";
    request.titleId = kTitle;
    // Akeno's own harmless test file, so the label may allow it; real mods need the catalogue.
    request.catalogueStatus = providers::CompatibilityStatus::Verified;
    request.catalogueInstallable = true;
    auto stored = install::storeMod(request, env);
    if (!stored) return failed("Store:    ", stored.error().describe());
    text += strings::concat("Store:    PASSED - ", stored->files.size(), " file(s) kept in ",
                            (context.paths().mods() / kTitle / downloadId).string(), "\n");

    auto applied = install::applyOverlay(target, env);
    if (!applied) return failed("Apply:    ", applied.error().describe());
    const auto& file = stored->files.front();
    const std::filesystem::path placed = applied->backport / file.installPath;
    auto hash = security::sha256File(placed);
    if (!hash || hash.value() != file.sha256) return failed("Apply:    ", "the file is not in place: " + placed.string());
    text += strings::concat("Apply:    PASSED - ", placed.string(), " in place, SHA-256 matches\n");

    auto vanilla = install::setVanilla(target, env);
    if (!vanilla) return failed("Vanilla:  ", vanilla.error().describe());
    struct stat info {};
    if (::lstat(applied->backport.c_str(), &info) == 0) return failed("Vanilla:  ", "the overlay is still there");
    text += "Vanilla:  PASSED - the overlay was removed\n";

    auto removed = install::removeStoredMod(env, kTitle, downloadId);
    if (!removed) return failed("Remove:   ", removed.error().describe());
    text += "Remove:   PASSED - the stored copy was deleted\n";
    return text;
}

}  // namespace

DownloadTestSpec builtinDownloadTest() {
    return DownloadTestSpec{
        // HEAD = the repository's default branch, whatever it is called.
        "https://raw.githubusercontent.com/YTDxDAkeno/AKENO-PS5-MOD-MANAGER/HEAD/assets/test/download-test.zip",
        AKENO_DOWNLOAD_TEST_SHA256, AKENO_DOWNLOAD_TEST_SIZE, std::nullopt, false};
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
        if (passed) {
            result += "\n\n" + checkTestArchive(context, queued->id, file.value(), spec.hardLinks, passed);
            if (passed && spec.overlayTest) result += "\n" + overlayTest(context, queued->id, file.value(), passed);
        }
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
