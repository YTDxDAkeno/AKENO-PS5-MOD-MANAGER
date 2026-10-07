// SPDX-License-Identifier: GPL-3.0-or-later
// All Phase 1 screens. Each one renders from AppViewState and requests work via IAppCommands.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "akeno/ui/Screen.hpp"

namespace akeno::ui {

class HomeScreen final : public Screen {
public:
    std::string title() const override { return "Home"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    bool animating(const UiEnv& env) const override;

private:
    FocusList actions_;
};

class GameLibraryScreen final : public Screen {
public:
    static constexpr int kColumns = 5;
    static constexpr int kVisibleRows = 2;
    static constexpr int kIconSize = 180;

    std::string title() const override { return "Games"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool animating(const UiEnv& env) const override { return env.state.library.loading; }

    const FocusGrid& grid() const { return grid_; }

private:
    FocusGrid grid_;
};

class GameDetailScreen final : public Screen {
public:
    static constexpr int kIconSize = 400;

    explicit GameDetailScreen(std::string titleId) : titleId_(std::move(titleId)) {}
    std::string title() const override { return "Game details"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    const std::string& titleId() const { return titleId_; }

private:
    std::string titleId_;
    FocusList actions_;
};

// Tabs whose features belong to later phases. They say so plainly instead of pretending.
class PlannedFeatureScreen final : public Screen {
public:
    PlannedFeatureScreen(std::string heading, std::string phase, std::vector<std::string> lines)
        : heading_(std::move(heading)), phase_(std::move(phase)), lines_(std::move(lines)) {}
    std::string title() const override { return heading_; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;

private:
    std::string heading_;
    std::string phase_;
    std::vector<std::string> lines_;
};

class SettingsScreen final : public Screen {
public:
    enum class Item {
        ShowPs4,
        ShowHomebrew,
        LibrarySort,
        DebugLogging,
        ShadowMountPort,
        RunSystemCheck,
        ViewLog,
        ExportDiagnostics,
        FirstRunGuide,
        Exit,
    };
    static constexpr int kItemCount = 10;

    std::string title() const override { return "Settings"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    const FocusList& list() const { return list_; }

private:
    void save(UiEnv& env, const database::Settings& settings, const std::string& message);
    FocusList list_;
};

class AboutScreen final : public Screen {
public:
    std::string title() const override { return "About"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;

private:
    int scroll_ = 0;
};

class LogViewerScreen final : public Screen {
public:
    std::string title() const override { return "Log"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;

private:
    int offsetFromEnd_ = 0;  // 0 = newest lines visible
};

// Full-screen startup check. Shows each result as it arrives, then the Safe Mode summary.
class SystemCheckScreen final : public Screen {
public:
    std::string title() const override { return "System Check"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool fullScreen() const override { return true; }
    bool animating(const UiEnv& env) const override { return env.state.systemCheck.running; }
};

// First-run guide (five steps).
class WizardScreen final : public Screen {
public:
    static constexpr int kStepCount = 7;  // welcome, five steps, ready

    std::string title() const override { return "Welcome"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool fullScreen() const override { return true; }
    bool animating(const UiEnv& env) const override;
    int step() const { return step_; }

private:
    int step_ = 0;
    bool libraryRequested_ = false;
};

// Shown first when the previous session was interrupted during an operation.
class RecoveryScreen final : public Screen {
public:
    std::string title() const override { return "Recovery"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool fullScreen() const override { return true; }

private:
    FocusList actions_;
};

// Installs the tab roots and the startup flows in the right order.
void setupScreens(ScreenHost& host, const AppViewState& state);

}  // namespace akeno::ui
