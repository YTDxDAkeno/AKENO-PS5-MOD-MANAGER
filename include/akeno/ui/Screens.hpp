// SPDX-License-Identifier: GPL-3.0-or-later
// All Phase 1 screens. Each one renders from AppViewState and requests work via IAppCommands.
#pragma once

#include <memory>
#include <optional>
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
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    const std::string& titleId() const { return titleId_; }

private:
    std::string titleId_;
    FocusList actions_;
    bool catalogRequested_ = false;
    ChoiceRequest confirmVanilla_;
};

// Discover tab: the games of the Akeno Catalogue, installed games first.
class DiscoverScreen final : public Screen {
public:
    static constexpr int kVisibleRows = 5;

    std::string title() const override { return "Discover"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool animating(const UiEnv& env) const override { return env.state.catalog.loading; }
    const FocusList& list() const { return list_; }

private:
    FocusList list_;
    std::optional<std::string> requestedSource_;
};

// The mods of one catalogue game, with order, search and compatibility labels for the
// installed version (when the game is installed).
class ModBrowserScreen final : public Screen {
public:
    static constexpr int kVisibleRows = 5;
    static constexpr int kThumbWidth = 192;
    static constexpr int kThumbHeight = 108;

    ModBrowserScreen(std::string providerGameId, std::string gameName, std::optional<providers::GameContext> game);
    std::string title() const override { return "Mods"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool animating(const UiEnv& env) const override { return env.state.modList.loading; }
    const providers::SearchQuery& query() const { return query_; }
    const FocusList& list() const { return list_; }

private:
    std::string gameName_;
    providers::SearchQuery query_;
    FocusList list_;
    TextRequest search_;
    std::vector<std::size_t> previousPages_;  // offsets of the pages before this one
};

class ModDetailScreen final : public Screen {
public:
    static constexpr int kHeroWidth = 640;
    static constexpr int kHeroHeight = 360;

    ModDetailScreen(providers::ModRef ref, std::string gameName, std::optional<providers::GameContext> game);
    std::string title() const override { return "Mod details"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool animating(const UiEnv& env) const override { return env.state.modDetail.loading; }
    const providers::ModRef& ref() const { return ref_; }
    int scroll() const { return scroll_; }

private:
    const providers::ModDetails* details(const UiEnv& env) const;

    providers::ModRef ref_;
    std::string gameName_;
    std::optional<providers::GameContext> game_;
    FocusList actions_;
    int scroll_ = 0;
    int maxScroll_ = 0;
    bool requested_ = false;
    ChoiceRequest confirmExperimental_;
};

// Downloads tab: progress, pause/resume/retry and removal.
class DownloadsScreen final : public Screen {
public:
    static constexpr int kVisibleRows = 5;

    std::string title() const override { return "Downloads"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool animating(const UiEnv& env) const override;
    const FocusList& list() const { return list_; }

private:
    FocusList list_;
    ChoiceRequest confirmRemove_;
    std::string removeId_;
};

// Every stored mod: turn on or off, remove, or switch a game to Vanilla.
class InstalledModsScreen final : public Screen {
public:
    static constexpr int kVisibleRows = 6;

    std::string title() const override { return "Installed Mods"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;

private:
    FocusList list_;
    ChoiceRequest confirmRemove_;
    ChoiceRequest confirmVanilla_;
    InstalledModRow pending_;
};

// The result of checking a downloaded mod: findings, conflicts, the dry-run install plan and
// the file list. Starts the check (or shows the stored result) when opened.
class ModCheckScreen final : public Screen {
public:
    explicit ModCheckScreen(std::string downloadId) : downloadId_(std::move(downloadId)) {}
    std::string title() const override { return "Check"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool animating(const UiEnv& env) const override { return env.state.check.running; }
    const std::string& downloadId() const { return downloadId_; }
    int scroll() const { return scroll_; }

private:
    std::string downloadId_;
    bool requested_ = false;
    int scroll_ = 0;
    int maxScroll_ = 0;
    ChoiceRequest confirmInstall_;
};

// A yes/no question. Cancel is focused first; CIRCLE also cancels.
class ConfirmScreen final : public Screen {
public:
    ConfirmScreen(std::string heading, std::vector<std::string> lines, std::string confirmLabel,
                  std::function<void(bool)> done, bool dangerous = false);
    ~ConfirmScreen() override;
    std::string title() const override { return heading_; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool fullScreen() const override { return true; }

private:
    void finish(bool confirmed);
    std::string heading_;
    std::vector<std::string> lines_;
    std::string confirmLabel_;
    std::function<void(bool)> done_;
    bool dangerous_;
    FocusList buttons_;
};

class ScreenshotViewerScreen final : public Screen {
public:
    ScreenshotViewerScreen(std::vector<providers::Screenshot> screenshots, int index)
        : screenshots_(std::move(screenshots)), index_(index) {}
    std::string title() const override { return "Screenshots"; }
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    bool fullScreen() const override { return true; }
    int index() const { return index_; }

private:
    std::vector<providers::Screenshot> screenshots_;
    int index_ = 0;
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
        CatalogueUrl,
        GameBanana,
        RunSystemCheck,
        ViewLog,
        ExportDiagnostics,
        FirstRunGuide,
        Exit,
    };
    static constexpr int kItemCount = 12;

    std::string title() const override { return "Settings"; }
    void update(UiEnv& env) override;
    NavRequest handle(Action action, UiEnv& env) override;
    void render(ICanvas& canvas, UiEnv& env) override;
    std::vector<ButtonHint> hints(const UiEnv& env) const override;
    const FocusList& list() const { return list_; }

private:
    void save(UiEnv& env, const database::Settings& settings, const std::string& message);
    FocusList list_;
    TextRequest catalogueUrl_;
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
