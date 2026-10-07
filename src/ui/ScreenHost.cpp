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

std::function<void(std::optional<std::string>)> TextRequest::callback() {
    slot_->pending = true;
    slot_->ready = false;
    slot_->text.reset();
    auto slot = slot_;
    return [slot](std::optional<std::string> text) {
        slot->pending = false;
        slot->ready = true;
        slot->text = std::move(text);
    };
}

bool TextRequest::take(std::optional<std::string>& text) {
    if (!slot_->ready) return false;
    slot_->ready = false;
    text = std::move(slot_->text);
    slot_->text.reset();
    return true;
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
    if (env.state.textEntry.active) {
        return;  // the text entry owns the input until it is finished
    }
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
    if (env.state.textEntry.active && !env.state.textEntry.systemKeyboard) return true;  // caret
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
    constexpr int kMaxTextWidth = 1300;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        Color accent = theme::kAccent;
        if (it->kind == ToastKind::Success) accent = theme::kOk;
        if (it->kind == ToastKind::Warning) accent = theme::kWarning;
        if (it->kind == ToastKind::Error) accent = theme::kError;
        // Up to two lines; anything longer is shortened at the end of the second line.
        auto lines = wrapText(canvas, it->text, kMaxTextWidth, FontRole::Body, false);
        if (lines.size() > 2) {
            for (std::size_t i = 2; i < lines.size(); ++i) lines[1] += " " + lines[i];
            lines.resize(2);
        }
        int textWidth = 0;
        for (const auto& line : lines) {
            textWidth = std::max(textWidth, canvas.measureText(line, FontRole::Body, false).w);
        }
        const int width = std::min(kMaxTextWidth, textWidth) + 80;
        const int height = lines.size() > 1 ? 120 : 76;
        const Rect box{theme::kScreenWidth - theme::kMargin - width, y - height, width, height};
        canvas.fillRoundedRect(box, 14, theme::kPanelRaised);
        canvas.fillRoundedRect({box.x, box.y, 10, box.h}, 5, accent);
        const int lineHeight = 44;
        int lineY = box.y + (box.h - lineHeight * static_cast<int>(lines.size())) / 2;
        for (const auto& line : lines) {
            canvas.drawText(line, {box.x + 36, lineY, box.w - 56, lineHeight},
                            TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false});
            lineY += lineHeight;
        }
        y -= height + 16;
    }
}

void ScreenHost::renderTextEntry(ICanvas& canvas, const UiEnv& env) {
    const TextEntryView& entry = env.state.textEntry;
    canvas.fillRect({0, 0, theme::kScreenWidth, theme::kScreenHeight}, Color{0, 0, 0, 170});
    const Rect box{(theme::kScreenWidth - 1300) / 2, 300, 1300, 380};
    draw::panel(canvas, box, true);
    canvas.fillRoundedRect({box.x, box.y, box.w, 10}, 5, theme::kAccent);
    canvas.drawText(entry.prompt, {box.x + 60, box.y + 40, box.w - 120, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    const Rect field{box.x + 60, box.y + 130, box.w - 120, 90};
    canvas.fillRoundedRect(field, 12, theme::kBackground);
    canvas.strokeRoundedRect(field, 12, 3, theme::kAccent);
    std::string shown = entry.text;
    const bool caretOn = !entry.systemKeyboard && static_cast<long long>(env.time * 2.0) % 2 == 0;
    if (caretOn) shown += "|";
    if (shown.empty() && entry.systemKeyboard) shown = " ";
    // Keep the end of long text visible: drop characters from the front until it fits.
    std::string_view visible = shown;
    while (visible.size() > 1 && canvas.measureText(visible, FontRole::Body, false).w > field.w - 60) {
        std::size_t cut = 1;
        while (cut < visible.size() && (static_cast<unsigned char>(visible[cut]) & 0xC0) == 0x80) ++cut;
        visible.remove_prefix(cut);
    }
    canvas.drawText(visible, {field.x + 30, field.y, field.w - 60, field.h},
                    TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false});
    const std::string_view help = entry.systemKeyboard
                                      ? "Type with the console keyboard and choose OK. Cancel returns without changes."
                                      : "Type, then press ENTER. ESC cancels.";
    drawWrappedText(canvas, help, {box.x + 60, field.bottom() + 30, box.w - 120, 100},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false}, 40, 2);
}

void ScreenHost::render(ICanvas& canvas, UiEnv& env) {
    Screen* screen = top();
    if (screen != nullptr) {
        screen->update(env);
        screen = top();
    }
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
    if (env.state.textEntry.active) {
        renderTextEntry(canvas, env);
    }
    renderToasts(canvas, env.time);
}

}  // namespace akeno::ui
