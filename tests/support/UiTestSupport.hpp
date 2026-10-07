// SPDX-License-Identifier: GPL-3.0-or-later
// Test doubles for the user interface: a canvas that records what was drawn and a command
// implementation that records what was requested.
#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "akeno/ui/Screens.hpp"

namespace akeno::test {

class RecordingCanvas final : public ui::ICanvas {
public:
    std::vector<std::string> texts;
    std::vector<std::string> images;
    int rects = 0;
    std::vector<std::string> availableImages;  // keys drawImage() reports as ready

    void fillRect(const ui::Rect&, ui::Color) override { ++rects; }
    void fillRoundedRect(const ui::Rect&, int, ui::Color) override { ++rects; }
    void strokeRoundedRect(const ui::Rect&, int, int, ui::Color) override { ++rects; }
    void fillCircle(int, int, int, ui::Color) override { ++rects; }
    void drawText(std::string_view text, const ui::Rect&, const ui::TextStyle&) override { texts.emplace_back(text); }
    ui::Size measureText(std::string_view text, ui::FontRole, bool) override {
        return {static_cast<int>(text.size()) * 15, 30};
    }
    bool drawImage(const std::string& key, const ui::Rect&) override {
        images.push_back(key);
        for (const auto& available : availableImages) {
            if (available == key) return true;
        }
        return false;
    }

    bool hasText(std::string_view needle) const {
        for (const auto& text : texts) {
            if (text.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    void clear() {
        texts.clear();
        images.clear();
        rects = 0;
    }
};

class FakeCommands final : public ui::IAppCommands {
public:
    explicit FakeCommands(ui::AppViewState& state) : state_(state) {}

    int systemChecks = 0;
    int libraryRefreshes = 0;
    int saves = 0;
    int cleanups = 0;
    int postpones = 0;
    bool quit = false;
    bool failSave = false;
    std::vector<logging::LogRecord> logs;
    int catalogLoads = 0;
    bool lastCatalogForce = false;
    std::vector<providers::SearchQuery> listRequests;
    std::vector<providers::ModRef> detailRequests;
    std::vector<std::string> remoteImages;  // urls requested
    struct DownloadCall {
        providers::ModRef ref;
        bool confirmed = false;
    };
    std::vector<DownloadCall> downloadStarts;
    std::vector<std::string> paused;
    std::vector<std::string> resumed;
    std::vector<std::string> removed;
    std::vector<std::pair<std::string, bool>> checks;  // id, again
    int checkCancels = 0;
    std::string textPrompt;
    std::string textInitial;
    std::function<void(std::optional<std::string>)> textDone;

    void runSystemCheck() override { ++systemChecks; }
    void refreshLibrary() override { ++libraryRefreshes; }
    Status saveSettings(const database::Settings& settings) override {
        if (failSave) return makeError(ErrorCode::Database, "database unavailable");
        ++saves;
        state_.settings = settings;
        return {};
    }
    Result<std::string> exportDiagnostics() override { return std::string("/data/akeno-mod-manager/logs/d.txt"); }
    std::vector<logging::LogRecord> recentLogs() override { return logs; }
    Status cleanInterruptedOperation() override {
        ++cleanups;
        state_.recovery.reset();
        return {};
    }
    void postponeRecovery() override { ++postpones; }
    std::string gameIconKey(const games::GameInfo& game, int size) override {
        return game.hasIcon ? "icon:" + game.titleId + ":" + std::to_string(size) : std::string();
    }
    std::string remoteImageKey(const std::string& url, int width, int height) override {
        if (url.empty()) return {};
        remoteImages.push_back(url);
        return "remote:" + url + ":" + std::to_string(width) + "x" + std::to_string(height);
    }
    void requestQuit() override { quit = true; }

    // Records the request and marks the views as loading, like the real controller.
    void loadCatalogGames(bool forceRefresh) override {
        ++catalogLoads;
        lastCatalogForce = forceRefresh;
        state_.catalog.loading = true;
    }
    void loadModList(const providers::SearchQuery& query) override {
        listRequests.push_back(query);
        state_.modList.query = query;
        state_.modList.loading = true;
        state_.modList.error.reset();
        ++state_.modList.request;
    }
    void loadModDetails(const providers::ModRef& ref, const std::optional<providers::GameContext>&) override {
        detailRequests.push_back(ref);
        state_.modDetail.ref = ref;
        state_.modDetail.loading = true;
        ++state_.modDetail.request;
    }
    void startDownload(const providers::ModRef& ref, const std::optional<providers::GameContext>&,
                       bool confirmed) override {
        downloadStarts.push_back({ref, confirmed});
    }
    Status pauseDownload(const std::string& id) override {
        paused.push_back(id);
        return {};
    }
    Status resumeDownload(const std::string& id) override {
        resumed.push_back(id);
        return {};
    }
    Status removeDownload(const std::string& id) override {
        removed.push_back(id);
        auto& items = state_.downloads.items;
        items.erase(std::remove_if(items.begin(), items.end(), [&](const auto& i) { return i.record.id == id; }),
                    items.end());
        return {};
    }
    void checkDownload(const std::string& id, bool again) override {
        checks.emplace_back(id, again);
        state_.check = ui::ModCheckView{};
        state_.check.downloadId = id;
        state_.check.running = true;
    }
    void cancelCheck() override { ++checkCancels; }
    void requestTextInput(const std::string& prompt, const std::string& initial,
                          std::function<void(std::optional<std::string>)> done) override {
        textPrompt = prompt;
        textInitial = initial;
        textDone = std::move(done);
        state_.textEntry.active = true;
        state_.textEntry.prompt = prompt;
        state_.textEntry.text = initial;
    }
    // Completes the pending text request (nullopt = cancelled).
    void completeText(std::optional<std::string> text) {
        state_.textEntry = ui::TextEntryView{};
        auto done = std::move(textDone);
        textDone = nullptr;
        if (done) done(std::move(text));
    }

private:
    ui::AppViewState& state_;
};

// Bundles state, commands, environment and toasts for a test.
struct UiHarness {
    ui::AppViewState state;
    FakeCommands commands{state};
    std::vector<std::string> toasts;
    ui::UiEnv env{state, commands, 1.0, [this](std::string text, ui::ToastKind) { toasts.push_back(std::move(text)); }};

    bool toasted(std::string_view needle) const {
        for (const auto& toast : toasts) {
            if (toast.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

}  // namespace akeno::test
