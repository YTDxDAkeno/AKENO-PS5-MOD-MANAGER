// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

// ---------------------------------------------------------------- PlannedFeatureScreen

NavRequest PlannedFeatureScreen::handle(Action /*action*/, UiEnv& /*env*/) { return NavRequest::none(); }

std::vector<ButtonHint> PlannedFeatureScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L1R1, "Switch tab"}};
}

void PlannedFeatureScreen::render(ICanvas& canvas, UiEnv& /*env*/) {
    const Rect content = theme::kContent;
    canvas.drawText(heading_, {content.x, content.y, content.w, 64},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
    draw::badge(canvas, content.x, content.y + 84, "NOT AVAILABLE IN THIS VERSION", theme::kNeutral, FontRole::Caption);
    canvas.drawText(phase_, {content.x, content.y + 150, content.w, 50},
                    TextStyle{FontRole::Body, theme::kAccent, TextAlign::Left, true});
    const Rect box{content.x, content.y + 220, content.w, 480};
    draw::panel(canvas, box);
    int y = box.y + 36;
    for (const auto& line : lines_) {
        canvas.fillCircle(box.x + 50, y + 22, 7, theme::kTextSecondary);
        int used = drawWrappedText(canvas, line, {box.x + 80, y, box.w - 120, 88},
                                   TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false}, 44, 2);
        y += std::max(1, used) * 44 + 24;
        if (y > box.bottom() - 40) break;
    }
}

// ---------------------------------------------------------------- AboutScreen

NavRequest AboutScreen::handle(Action action, UiEnv& /*env*/) {
    if (action == Action::Down || action == Action::PageDown) scroll_ = std::min(scroll_ + 1, 6);
    if (action == Action::Up || action == Action::PageUp) scroll_ = std::max(scroll_ - 1, 0);
    return NavRequest::none();
}

std::vector<ButtonHint> AboutScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L1R1, "Switch tab"}};
}

void AboutScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const AboutInfo& about = env.state.about;
    canvas.drawText("Akeno PS5 Mod Manager", {content.x, content.y, content.w, 64},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
    canvas.drawText(strings::concat("Version ", about.version, "  (", about.target, ", ", about.revision, ")"),
                    {content.x, content.y + 70, content.w, 44},
                    TextStyle{FontRole::Body, theme::kAccent, TextAlign::Left, false});

    // The testing status can need up to three lines; the rows below move with it.
    const std::string statusText = "Testing status: " + about.testingStatus;
    const int statusLines = std::clamp(
        static_cast<int>(wrapText(canvas, statusText, content.w - 80, FontRole::Body, false).size()), 1, 3);
    const Rect status{content.x, content.y + 130, content.w, statusLines * 40 + 40};
    draw::panel(canvas, status);
    canvas.fillRoundedRect({status.x, status.y, 10, status.h}, 5, theme::kWarning);
    drawWrappedText(canvas, statusText, {status.x + 40, status.y + 20, status.w - 80, statusLines * 40},
                    TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false}, 40, 3);

    std::vector<std::pair<std::string, std::string>> rows{
        {"Platform", about.platformName},
        {"Firmware", about.firmware},
        {"Data directory", about.dataRoot},
        {"Network", about.networkStack},
        {"Database", about.databaseEngine},
        {"Compiler", about.compiler},
        {"License", "GPL-3.0-or-later. No analytics, no telemetry."},
        {"Built on", "ShadowMountPlus (drakmor), ps5-payload-sdk (John Tornblom), SDL2 PS5 port, libcurl, OpenSSL, "
                     "SQLite, nlohmann/json, DejaVu fonts"},
        {"Notices", "See THIRD_PARTY_NOTICES.md in the source repository."},
    };
    int y = status.bottom() + 30;
    const int first = std::min(scroll_, static_cast<int>(rows.size()) - 1);
    for (int i = first; i < static_cast<int>(rows.size()); ++i) {
        canvas.drawText(rows[static_cast<std::size_t>(i)].first, {content.x, y, 300, 44},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
        int used = drawWrappedText(canvas, rows[static_cast<std::size_t>(i)].second,
                                   {content.x + 300, y, content.w - 300, 88},
                                   TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false}, 44, 2);
        y += std::max(1, used) * 44 + 14;
        if (y > content.bottom() - 50) break;
    }
}

// ---------------------------------------------------------------- LogViewerScreen

namespace {
constexpr int kLogLineHeight = 34;
constexpr int kLogVisibleLines = 20;
}  // namespace

NavRequest LogViewerScreen::handle(Action action, UiEnv& env) {
    switch (action) {
        case Action::Back:
            return NavRequest::pop();
        case Action::Up: ++offsetFromEnd_; break;
        case Action::Down: offsetFromEnd_ = std::max(0, offsetFromEnd_ - 1); break;
        case Action::PageUp: offsetFromEnd_ += kLogVisibleLines; break;
        case Action::PageDown: offsetFromEnd_ = std::max(0, offsetFromEnd_ - kLogVisibleLines); break;
        case Action::Options: {
            auto exported = env.commands.exportDiagnostics();
            if (exported) {
                env.showToast("Diagnostic log written to " + exported.value(), ToastKind::Success);
            } else {
                env.showToast("Export failed: " + exported.error().message, ToastKind::Error);
            }
            break;
        }
        default: break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> LogViewerScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L2R2, "Page"},
            {ButtonHint::Button::Options, "Export diagnostic log"},
            {ButtonHint::Button::Circle, "Back"}};
}

void LogViewerScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    canvas.drawText("Log", {content.x, content.y, content.w, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    const auto records = env.commands.recentLogs();
    const int total = static_cast<int>(records.size());
    offsetFromEnd_ = std::clamp(offsetFromEnd_, 0, std::max(0, total - kLogVisibleLines));
    const int end = total - offsetFromEnd_;
    const int start = std::max(0, end - kLogVisibleLines);
    const Rect box{content.x, content.y + 70, content.w, kLogVisibleLines * kLogLineHeight + 24};
    draw::panel(canvas, box);
    int y = box.y + 12;
    for (int i = start; i < end; ++i) {
        const auto& record = records[static_cast<std::size_t>(i)];
        Color color = theme::kTextSecondary;
        if (record.level == logging::LogLevel::Warn) color = theme::kWarning;
        if (record.level == logging::LogLevel::Error) color = theme::kError;
        if (record.level == logging::LogLevel::Info) color = theme::kTextPrimary;
        std::string line = record.timestamp.size() >= 19 ? record.timestamp.substr(11, 8) : record.timestamp;
        line += strings::concat("  [", record.category, "] ", record.message);
        canvas.drawText(line, {box.x + 24, y, box.w - 48, kLogLineHeight},
                        TextStyle{FontRole::Small, color, TextAlign::Left, false});
        y += kLogLineHeight;
    }
    if (total == 0) {
        canvas.drawText("No log messages yet.", box, TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Center});
    }
}

}  // namespace akeno::ui
