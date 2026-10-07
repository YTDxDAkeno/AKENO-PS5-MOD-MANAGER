// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "akeno/ui/AppModel.hpp"
#include "akeno/ui/Canvas.hpp"
#include "akeno/ui/Input.hpp"
#include "akeno/ui/Widgets.hpp"

namespace akeno::ui {

struct UiEnv {
    const AppViewState& state;
    IAppCommands& commands;
    double time = 0.0;                                       // seconds since start
    std::function<void(std::string, ToastKind)> toast;       // may be empty in tests

    void showToast(std::string text, ToastKind kind = ToastKind::Info) const {
        if (toast) toast(std::move(text), kind);
    }
};

class Screen;

enum class Tab { Home, Games, Discover, Downloads, InstalledMods, Updates, Settings, About };
inline constexpr int kTabCount = 8;
std::string_view tabLabel(Tab tab) noexcept;

struct NavRequest {
    enum class Kind { None, Push, Pop, Replace, SwitchTab };
    Kind kind = Kind::None;
    std::unique_ptr<Screen> screen;
    Tab tab = Tab::Home;

    static NavRequest none() { return {}; }
    static NavRequest push(std::unique_ptr<Screen> screen) { return {Kind::Push, std::move(screen), Tab::Home}; }
    static NavRequest replace(std::unique_ptr<Screen> screen) { return {Kind::Replace, std::move(screen), Tab::Home}; }
    static NavRequest pop() { return {Kind::Pop, nullptr, Tab::Home}; }
    static NavRequest switchTab(Tab tab) { return {Kind::SwitchTab, nullptr, tab}; }
};

class Screen {
public:
    virtual ~Screen() = default;

    virtual std::string title() const = 0;
    virtual NavRequest handle(Action action, UiEnv& env) = 0;
    // Draws into theme::kContent, or the whole screen when fullScreen() is true.
    virtual void render(ICanvas& canvas, UiEnv& env) = 0;
    virtual std::vector<ButtonHint> hints(const UiEnv& env) const;
    // Full-screen flows (first-run guide, system check, recovery) hide the tab bar and receive
    // every input, including L1/R1.
    virtual bool fullScreen() const { return false; }
    // True while something moves on screen (spinners), so the loop keeps redrawing.
    virtual bool animating(const UiEnv& /*env*/) const { return false; }
};

struct Toast {
    std::string text;
    ToastKind kind = ToastKind::Info;
    double expiresAt = 0.0;
};

// Owns the navigation state: one stack per tab plus full-screen flows on top.
class ScreenHost {
public:
    ScreenHost();

    void setTabRoot(Tab tab, std::unique_ptr<Screen> screen);
    void pushFlow(std::unique_ptr<Screen> screen);

    void handle(Action action, UiEnv& env);
    void render(ICanvas& canvas, UiEnv& env);
    bool animating(const UiEnv& env) const;

    Tab currentTab() const { return current_; }
    void switchTab(Tab tab) { current_ = tab; }
    Screen* top();
    std::size_t flowDepth() const { return flows_.size(); }
    std::size_t tabDepth(Tab tab) const { return tabs_[static_cast<std::size_t>(tab)].size(); }

    void addToast(std::string text, ToastKind kind, double now);
    const std::deque<Toast>& toasts() const { return toasts_; }

private:
    void apply(NavRequest request, std::vector<std::unique_ptr<Screen>>& stack, bool isFlow);
    void renderChrome(ICanvas& canvas, UiEnv& env);
    void renderToasts(ICanvas& canvas, double now);

    std::array<std::vector<std::unique_ptr<Screen>>, kTabCount> tabs_;
    std::vector<std::unique_ptr<Screen>> flows_;
    Tab current_ = Tab::Home;
    std::deque<Toast> toasts_;
};

}  // namespace akeno::ui
