// SPDX-License-Identifier: GPL-3.0-or-later
// Implements IAppCommands on top of AppContext. Owns AppViewState, which is only modified on
// the UI thread; background work posts its results through MainThreadQueue.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "akeno/app/AppContext.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/ui/AppModel.hpp"

namespace akeno::ui {

// Decodes images off the UI thread. `fetch` runs on a worker and returns encoded bytes.
class IImageLoader {
public:
    virtual ~IImageLoader() = default;
    // Idempotent and cheap for keys that are already loading, loaded or failed.
    virtual void ensure(const std::string& key, int size, std::function<Result<std::string>()> fetch) = 0;
    // Allows images that failed to load to be tried again (after a library refresh).
    virtual void retryFailed() = 0;
};

class AppController final : public IAppCommands {
public:
    AppController(app::AppContext& context, TaskRunner& tasks, MainThreadQueue& mainQueue, IImageLoader* images);

    const AppViewState& state() const { return state_; }
    bool quitRequested() const { return quit_; }

    // Starts the system check; the library refresh follows automatically when it is possible.
    void start();

    void runSystemCheck() override;
    void refreshLibrary() override;
    Status saveSettings(const database::Settings& settings) override;
    Result<std::string> exportDiagnostics() override;
    std::vector<logging::LogRecord> recentLogs() override;
    Status cleanInterruptedOperation() override;
    void postponeRecovery() override;
    std::string gameIconKey(const games::GameInfo& game, int size) override;
    void requestQuit() override { quit_ = true; }

private:
    void applyFilter();
    Result<std::string> fetchIcon(const games::GameInfo& game);

    app::AppContext& context_;
    TaskRunner& tasks_;
    MainThreadQueue& mainQueue_;
    IImageLoader* images_;
    AppViewState state_;
    std::vector<games::GameInfo> allGames_;
    bool quit_ = false;
};

}  // namespace akeno::ui
