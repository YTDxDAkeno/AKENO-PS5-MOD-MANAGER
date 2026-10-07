// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/AppController.hpp"

#include <system_error>

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/games/GameLibrary.hpp"
#include "akeno/network/CurlHttpClient.hpp"
#include "akeno/security/SafeName.hpp"

namespace akeno::ui {

using logging::logger;

AppController::AppController(app::AppContext& context, TaskRunner& tasks, MainThreadQueue& mainQueue,
                             IImageLoader* images)
    : context_(context), tasks_(tasks), mainQueue_(mainQueue), images_(images) {
    state_.settings = context_.settings();
    state_.settingsPersistent = context_.database() != nullptr;
    if (const auto& interrupted = context_.interruptedOperation()) {
        state_.recovery = RecoveryView{*interrupted, adviseRecovery(*interrupted)};
    }
    AboutInfo& about = state_.about;
    about.version = std::string(build::version());
    about.revision = std::string(build::gitRevision());
    about.target = std::string(build::target());
    about.compiler = std::string(build::compiler());
    about.testingStatus = std::string(build::testingStatus());
    about.platformName = std::string(context_.platform().name());
    about.firmware = context_.platform().firmware().display;
    about.dataRoot = context_.paths().root.string();
    about.networkStack = network::curlVersionDescription();
    about.databaseEngine = database::sqliteVersion();
    if (context_.databaseError()) {
        about.databaseEngine += " - unavailable: " + context_.databaseError()->message;
    }
}

void AppController::start() { runSystemCheck(); }

void AppController::runSystemCheck() {
    if (state_.systemCheck.running) {
        return;
    }
    state_.systemCheck.running = true;
    state_.systemCheck.results.clear();
    // Built on the UI thread so it captures a consistent copy of the settings; it then runs on a
    // worker. The checker only uses thread-safe services.
    auto checker = std::make_shared<app::SystemChecker>(context_.makeSystemChecker());
    tasks_.submit([this, checker] {
        app::SystemReport report = checker->run([this](const app::CheckResult& result) {
            mainQueue_.post([this, result] { state_.systemCheck.results.push_back(result); });
        });
        mainQueue_.post([this, report = std::move(report)]() mutable {
            state_.systemCheck.running = false;
            state_.systemCheck.results = report.checks;
            const bool libraryAvailable = report.features.gameLibrary.state == app::FeatureState::Available;
            state_.systemCheck.report = std::move(report);
            if (libraryAvailable && !state_.library.everLoaded) {
                refreshLibrary();
            }
        });
    });
}

void AppController::refreshLibrary() {
    games::GameLibrary* library = context_.library();
    if (library == nullptr) {
        state_.library.error = makeError(ErrorCode::Unavailable, "ShadowMountPlus is not configured.");
        return;
    }
    if (state_.library.loading) {
        return;
    }
    state_.library.loading = true;
    tasks_.submit([this, library] {
        auto snapshot = library->refresh();
        mainQueue_.post([this, snapshot = std::move(snapshot)]() mutable {
            state_.library.loading = false;
            if (!snapshot) {
                state_.library.error = snapshot.error();
                logger().warn("ui", "library refresh failed: " + snapshot.error().describe());
                return;
            }
            state_.library.error.reset();
            state_.library.everLoaded = true;
            state_.library.refreshedAt = snapshot->refreshedAt;
            state_.library.versionChanges = snapshot->versionChanges;
            allGames_ = std::move(snapshot->games);
            if (images_ != nullptr) images_->retryFailed();
            applyFilter();
        });
    });
}

void AppController::applyFilter() {
    games::LibraryFilter filter;
    filter.includePs4 = state_.settings.showPs4Games;
    filter.includeHomebrew = state_.settings.showHomebrew;
    filter.sort = state_.settings.librarySort;
    state_.library.totalGames = allGames_.size();
    state_.library.games = games::applyFilter(allGames_, filter);
}

Status AppController::saveSettings(const database::Settings& settings) {
    AKENO_TRY(context_.saveSettings(settings));
    state_.settings = settings;
    applyFilter();
    return {};
}

Result<std::string> AppController::exportDiagnostics() {
    const app::SystemReport* report = state_.systemCheck.report ? &*state_.systemCheck.report : nullptr;
    auto file = context_.exportDiagnostics(report);
    if (!file) {
        return std::move(file).error();
    }
    return file->string();
}

std::vector<logging::LogRecord> AppController::recentLogs() { return context_.logRing().snapshot(); }

Status AppController::cleanInterruptedOperation() {
    if (!state_.recovery) {
        return {};
    }
    AKENO_TRY(context_.journal().cleanStaging(state_.recovery->state, context_.paths().staging()));
    logger().info("recovery", "cleaned staging of interrupted operation " + state_.recovery->state.operationId);
    state_.recovery.reset();
    context_.clearInterruptedOperation();
    return {};
}

void AppController::postponeRecovery() {
    if (state_.recovery) {
        logger().info("recovery", "recovery postponed by the user: " + state_.recovery->state.operationId);
    }
}

Result<std::string> AppController::fetchIcon(const games::GameInfo& game) {
    // Runs on a worker. Icons are cached per title and version in cache/icons.
    const std::string fileName =
        game.titleId + "-" + security::toSafeFileComponent(game.version.empty() ? "unknown" : game.version) + ".png";
    const auto cacheFile = context_.paths().iconCache() / fileName;
    std::error_code ec;
    if (std::filesystem::exists(cacheFile, ec)) {
        auto cached = security::readFileBounded(cacheFile, limits::kMaxImageBytes);
        if (cached) {
            return cached;
        }
    }
    games::IGameDiscoveryProvider* provider = context_.gameProvider();
    if (provider == nullptr) {
        return makeError(ErrorCode::Unavailable, "No game provider.");
    }
    auto bytes = provider->loadIcon(game, games::IconSize::Full);
    if (!bytes) {
        return bytes;
    }
    auto stored = context_.fs().writeFileAtomic(cacheFile, bytes.value());
    if (!stored) {
        logger().warn("images", "could not cache icon: " + stored.error().describe());
    }
    return bytes;
}

std::string AppController::gameIconKey(const games::GameInfo& game, int size) {
    if (!game.hasIcon || !games::isValidTitleId(game.titleId)) {
        return {};
    }
    std::string key = strings::concat("icon:", game.titleId, ":", game.version, ":", size);
    if (images_ != nullptr) {
        images_->ensure(key, size, [this, game] { return fetchIcon(game); });
    }
    return key;
}

}  // namespace akeno::ui
