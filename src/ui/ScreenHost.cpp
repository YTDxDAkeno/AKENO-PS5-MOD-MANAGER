// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "akeno/ui/Screen.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

std::string_view tabLabel(Tab tab) noexcept {
    switch (tab) {
        case Tab::Home: return "HOME";
        case Tab::Games: return "GAMES";
        case Tab::Discover: return "DISCOVER";
        case Tab::Downloads: return "DOWNLOADS";
        case Tab::InstalledMods: return "INSTALLED MODS";
        case Tab::Updates: return "UPDATES";
        case Tab::Settings: return "SETTINGS";
        case Tab::About: return "ABOUT";
    }
    return "";
}

std::vector<ButtonHint> Screen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L1R1, "Switch tab"},
            {ButtonHint::Button::Circle, "Back"},
            {ButtonHint::Button::Cross, "Select"}};
}

ScreenHost::ScreenHost() = default;

void ScreenHost::setTabRoot(Tab tab, std::unique_ptr<Screen> screen) {
    auto& stack = tabs_[static_cast<std::size_t>(tab)];
    stack.clear();
    stack.push_back(std::move(screen));
}

void ScreenHost::pushFlow(std::unique_ptr<Screen> screen) { flows_.push_back(std::move(screen)); }

Screen* ScreenHost::top() {
    if (!flows_.empty()) {
        return flows_.back().get();
    }
    auto& stack = tabs_[static_cast<std::size_t>(current_)];
    return stack.empty() ? nullptr : stack.back().get();
}

void ScreenHost::apply(NavRequest request, std::vector<std::unique_ptr<Screen>>& stack, bool isFlow) {
    switch (request.kind) {
        case NavRequest::Kind::None:
            break;
        case NavRequest::Kind::Push:
            if (request.screen) stack.push_back(std::move(request.screen));
            break;
        case NavRequest::Kind::Replace:
            if (request.screen) {
                if (!stack.empty()) stack.pop_back();
                stack.push_back(std::move(request.screen));
            }
            break;
        case NavRequest::Kind::Pop:
            // A tab's root screen stays; flows may close completely.
            if (isFlow ? !stack.empty() : stack.size() > 1) stack.pop_back();
            break;
        case NavRequest::Kind::SwitchTab:
            if (isFlow) stack.clear();  // leaving a flow for a tab closes the flow
            current_ = request.tab;
            break;
    }
}

void ScreenHost::handle(Action action, UiEnv& env) {
    if (!flows_.empty()) {
        Screen* screen = flows_.back().get();
        apply(screen->handle(action, env), flows_, true);
        return;
    }
    auto& stack = tabs_[static_cast<std::size_t>(current_)];
    const bool fullScreenTop = !stack.empty() && stack.back()->fullScreen();
    if (!fullScreenTop && (action == Action::PrevTab || action == Action::NextTab)) {
        int index = static_cast<int>(current_) + (action == Action::NextTab ? 1 : -1);
        index = (index + kTabCount) % kTabCount;
        current_ = static_cast<Tab>(index);
        return;
    }
    if (stack.empty()) {
        return;
    }
    apply(stack.back()->handle(action, env), stack, false);
}

bool ScreenHost::animating(const UiEnv& env) const {
    if (!toasts_.empty()) return true;
    if (!flows_.empty()) return flows_.back()->animating(env);
    const auto& stack = tabs_[static_cast<std::size_t>(current_)];
    return !stack.empty() && stack.back()->animating(env);
}

void ScreenHost::addToast(std::string text, ToastKind kind, double now) {
    toasts_.push_back(Toast{std::move(text), kind, now + 3.5});
    while (toasts_.size() > 3) toasts_.pop_front();
}

void ScreenHost::renderChrome(ICanvas& canvas, UiEnv& env) {
    canvas.fillRect({0, 0, theme::kScreenWidth, theme::kScreenHeight}, theme::kBackground);
    canvas.fillRect(theme::kHeader, theme::kBackgroundTop);

    const Rect header = theme::kHeader;
    const int logoWidth = canvas.measureText("AKENO", FontRole::Title, true).w;
    canvas.drawText("AKENO", {theme::kMargin, header.y, logoWidth, header.h},
                    TextStyle{FontRole::Title, theme::kAccent, TextAlign::Left, true});
    canvas.drawText("MOD MANAGER", {theme::kMargin + logoWidth + 16, header.y, 500, header.h},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, false});

    // Status badges, right-aligned.
    int x = theme::kScreenWidth - theme::kMargin;
    const int badgeY = header.y + (header.h - (theme::fontSize(FontRole::Small) + 16)) / 2;
    auto placeBadge = [&](std::string_view label, Color color) {
        const int width = canvas.measureText(label, FontRole::Small, true).w + 28;
        x -= width;
        draw::badge(canvas, x, badgeY, label, color);
        x -= 16;
    };
    const auto& check = env.state.systemCheck;
    if (check.report) {
        placeBadge(check.report->features.safeMode() ? "SAFE MODE" : "INSTALLS ENABLED",
                   check.report->features.safeMode() ? theme::kWarning : theme::kOk);
        const auto* api = check.report->find(app::CheckId::ShadowMountApi);
        const bool connected = api != nullptr && api->status == app::CheckStatus::Ok;
        placeBadge(connected ? "SHADOWMOUNT" : "NO SHADOWMOUNT", connected ? theme::kOk : theme::kError);
    } else if (check.running) {
        placeBadge("CHECKING SYSTEM", theme::kNeutral);
    }

    // Tab bar.
    const Rect bar = theme::kTabBar;
    canvas.fillRect(bar, theme::kBackground);
    int tabX = theme::kMargin;
    for (int i = 0; i < kTabCount; ++i) {
        const Tab tab = static_cast<Tab>(i);
        const std::string_view label = tabLabel(tab);
        const bool active = tab == current_;
        const int width = canvas.measureText(label, FontRole::Caption, true).w;
        canvas.drawText(label, {tabX, bar.y, width, bar.h - 10},
                        TextStyle{FontRole::Caption, active ? theme::kTextPrimary : theme::kTextSecondary,
                                  TextAlign::Left, true});
        if (active) {
            canvas.fillRoundedRect({tabX, bar.bottom() - 14, width, 6}, 3, theme::kAccent);
        }
        tabX += width + 48;
    }
}

void ScreenHost::renderToasts(ICanvas& canvas, double now) {
    while (!toasts_.empty() && toasts_.front().expiresAt <= now) {
        toasts_.pop_front();
    }
    int y = theme::kFooter.y - 30;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        Color accent = theme::kAccent;
        if (it->kind == ToastKind::Success) accent = theme::kOk;
        if (it->kind == ToastKind::Warning) accent = theme::kWarning;
        if (it->kind == ToastKind::Error) accent = theme::kError;
        const int width = std::min(1100, canvas.measureText(it->text, FontRole::Body, false).w + 80);
        const Rect box{theme::kScreenWidth - theme::kMargin - width, y - 76, width, 76};
        canvas.fillRoundedRect(box, 14, theme::kPanelRaised);
        canvas.fillRoundedRect({box.x, box.y, 10, box.h}, 5, accent);
        canvas.drawText(it->text, {box.x + 36, box.y, box.w - 56, box.h},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false});
        y -= 92;
    }
}

void ScreenHost::render(ICanvas& canvas, UiEnv& env) {
    Screen* screen = top();
    const bool full = screen != nullptr && screen->fullScreen();
    if (full) {
        canvas.fillRect({0, 0, theme::kScreenWidth, theme::kScreenHeight}, theme::kBackground);
    } else {
        renderChrome(canvas, env);
    }
    if (screen != nullptr) {
        screen->render(canvas, env);
        draw::footerHints(canvas, screen->hints(env));
    }
    renderToasts(canvas, env.time);
}

}  // namespace akeno::ui
