// SPDX-License-Identifier: GPL-3.0-or-later
// Implements IAppCommands on top of AppContext. Owns AppViewState, which is only modified on
// the UI thread; background work posts its results through MainThreadQueue.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "akeno/app/AppContext.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/ui/AppModel.hpp"

namespace akeno::ui {

// Decodes images off the UI thread. `fetch` runs on a worker and returns encoded bytes.
class IImageLoader {
public:
    virtual ~IImageLoader() = default;
    // Idempotent and cheap for keys that are already loading, loaded or failed. The decoded
    // image is scaled to fit width x height (letterboxed, aspect ratio kept).
    virtual void ensure(const std::string& key, int width, int height,
                        std::function<Result<std::string>()> fetch) = 0;
    // Allows images that failed to load to be tried again (after a library refresh).
    virtual void retryFailed() = 0;
};

class AppController final : public IAppCommands {
public:
    AppController(app::AppContext& context, TaskRunner& tasks, MainThreadQueue& mainQueue, IImageLoader* images);
    // Stops the download engine (an active transfer continues at the next start).
    ~AppController() override;
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    const AppViewState& state() const { return state_; }
    bool quitRequested() const { return quit_; }

    // Starts the system check (the library refresh follows when it is possible) and the
    // download engine.
    void start();

    void runSystemCheck() override;
    void refreshLibrary() override;
    Status saveSettings(const database::Settings& settings) override;
    Result<std::string> exportDiagnostics() override;
    std::vector<logging::LogRecord> recentLogs() override;
    Status cleanInterruptedOperation() override;
    void postponeRecovery() override;
    std::string gameIconKey(const games::GameInfo& game, int size) override;
    std::string remoteImageKey(const std::string& url, int width, int height) override;
    void requestQuit() override { quit_ = true; }

    void loadCatalogGames(bool forceRefresh) override;
    void loadModList(const providers::SearchQuery& query) override;
    void loadModDetails(const providers::ModRef& ref, const std::optional<providers::GameContext>& game) override;

    void startDownload(const providers::ModRef& ref, const std::optional<providers::GameContext>& game,
                       bool confirmed) override;
    Status pauseDownload(const std::string& id) override;
    Status resumeDownload(const std::string& id) override;
    Status removeDownload(const std::string& id) override;
    void checkDownload(const std::string& id, bool again) override;
    void cancelCheck() override;
    void installChecked(const std::string& downloadId) override;
    void setGameVanilla(const std::string& titleId) override;
    InstalledModsSummary installedMods(const std::string& titleId) override;
    bool installBusy() const override { return installing_; }

    void requestTextInput(const std::string& prompt, const std::string& initial,
                          std::function<void(std::optional<std::string>)> done) override;
    // Text entry events from the input backend (UI thread).
    void appendTextInput(std::string_view utf8);
    void eraseTextInput();               // removes the last character
    void setTextInput(std::string text);
    void finishTextInput(bool accepted); // calls the pending callback

    static constexpr std::size_t kMaxTextInputBytes = 256;

private:
    void applyFilter();
    void resetCatalogView();
    void pruneImageCache();
    void refreshDownloads();
    void removeOrphanReports();
    void addNotice(std::string text, ToastKind kind);
    Result<std::string> fetchIcon(const games::GameInfo& game);
    Result<std::string> fetchRemoteImage(const std::string& url);

    app::AppContext& context_;
    TaskRunner& tasks_;
    MainThreadQueue& mainQueue_;
    IImageLoader* images_;
    AppViewState state_;
    std::vector<games::GameInfo> allGames_;
    std::uint64_t catalogGeneration_ = 0;  // bumped when the catalogue address changes
    CancellationToken listCancel_;
    CancellationToken detailCancel_;
    CancellationToken checkCancel_;
    std::unordered_map<std::string, std::string> urlKeys_;  // url -> sha256, memoised for drawing
    std::function<void(std::optional<std::string>)> textInputDone_;
    std::uint64_t noticeSerial_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> downloadsRefreshPending_ = std::make_shared<std::atomic<bool>>(false);
    bool quit_ = false;
    bool installing_ = false;  // one install or Vanilla at a time; never during a check
    std::unordered_map<std::string, InstalledModsSummary> modSummaries_;  // read once, dropped after changes
};

}  // namespace akeno::ui
