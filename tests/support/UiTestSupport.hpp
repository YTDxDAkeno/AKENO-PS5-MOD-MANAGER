// SPDX-License-Identifier: GPL-3.0-or-later
// Test doubles for the user interface: a canvas that records what was drawn and a command
// implementation that records what was requested.
#pragma once

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
    void requestQuit() override { quit = true; }

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
