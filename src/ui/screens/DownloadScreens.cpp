// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3 screens: the Downloads tab and the confirmation dialog.
#include <algorithm>
#include <array>
#include <cmath>

#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

using downloads::DownloadInfo;
using downloads::DownloadState;

namespace {

const char* stateLabel(DownloadState state) {
    switch (state) {
        case DownloadState::Queued: return "QUEUED";
        case DownloadState::Downloading: return "DOWNLOADING";
        case DownloadState::Paused: return "PAUSED";
        case DownloadState::Verifying: return "CHECKING";
        case DownloadState::Completed: return "DOWNLOADED";  // not "VERIFIED": that word is a compatibility label
        case DownloadState::Failed: return "FAILED";
    }
    return "";
}

Color stateColor(DownloadState state) {
    switch (state) {
        case DownloadState::Queued: return theme::kNeutral;
        case DownloadState::Downloading: return theme::kAccent;
        case DownloadState::Paused: return theme::kWarning;
        case DownloadState::Verifying: return theme::kAccent;
        case DownloadState::Completed: return theme::kOk;
        case DownloadState::Failed: return theme::kError;
    }
    return theme::kNeutral;
}

std::string formatDuration(double seconds) {
    if (seconds < 60) return strings::concat(static_cast<int>(std::ceil(seconds)), " s");
    if (seconds < 3600) return strings::concat(static_cast<int>(std::ceil(seconds / 60)), " min");
    return strings::concat(static_cast<int>(seconds / 3600), " h ", static_cast<int>(std::fmod(seconds, 3600) / 60), " min");
}

double fraction(const DownloadInfo& info) {
    const auto& record = info.record;
    if (record.request.expectedSize == 0) return 0.0;
    const std::uint64_t done = record.state == DownloadState::Verifying ? info.progress.verifiedBytes : record.bytesDone;
    return std::clamp(static_cast<double>(done) / static_cast<double>(record.request.expectedSize), 0.0, 1.0);
}

// The line under the name: what is happening and what comes next.
std::string statusLine(const DownloadInfo& info) {
    const auto& record = info.record;
    const std::string total = strings::formatBytes(record.request.expectedSize);
    const std::string done = strings::formatBytes(record.bytesDone);
    switch (record.state) {
        case DownloadState::Queued:
            return record.bytesDone > 0 ? strings::concat("Waiting to continue, ", done, " of ", total)
                                        : strings::concat("Waiting, ", total);
        case DownloadState::Downloading: {
            if (info.progress.retryInSeconds) {
                return strings::concat("Connection problem, trying again in ", formatDuration(*info.progress.retryInSeconds),
                                       ". ", record.error);
            }
            std::string line = strings::concat(done, " of ", total);
            if (info.progress.bytesPerSecond > 0) {
                line += "   " + strings::formatBytes(static_cast<std::uint64_t>(info.progress.bytesPerSecond)) + "/s";
            }
            if (info.progress.secondsLeft) line += "   about " + formatDuration(*info.progress.secondsLeft) + " left";
            return line;
        }
        case DownloadState::Paused: return strings::concat("Paused at ", done, " of ", total);
        case DownloadState::Verifying:
            return strings::concat("Checking the SHA-256 checksum, ", static_cast<int>(fraction(info) * 100), "%");
        case DownloadState::Completed:
            return strings::concat(total, ", SHA-256 checked. Installing comes in a later version.");
        case DownloadState::Failed: return record.error.empty() ? std::string("Failed.") : record.error;
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------- DownloadsScreen

void DownloadsScreen::update(UiEnv& env) {
    bool confirmed = false;
    if (confirmRemove_.take(confirmed) && confirmed && !removeId_.empty()) {
        const DownloadInfo* item = env.state.downloads.find(removeId_);
        const std::string name = item != nullptr ? item->record.request.displayName : std::string("the download");
        auto removed = env.commands.removeDownload(removeId_);
        env.showToast(removed ? "Removed " + name : "Could not remove: " + removed.error().message,
                      removed ? ToastKind::Info : ToastKind::Error);
        removeId_.clear();
    }
}

NavRequest DownloadsScreen::handle(Action action, UiEnv& env) {
    const auto& items = env.state.downloads.items;
    list_.setCount(static_cast<int>(items.size()));
    list_.setVisibleRows(kVisibleRows);
    if (list_.handle(action) || items.empty()) {
        return NavRequest::none();
    }
    // Newest first on screen.
    const DownloadInfo& item = items[items.size() - 1 - static_cast<std::size_t>(list_.focus())];
    const std::string& id = item.record.id;
    switch (action) {
        case Action::Confirm: {
            Status result;
            switch (item.record.state) {
                case DownloadState::Queued:
                case DownloadState::Downloading:
                case DownloadState::Verifying:
                    result = env.commands.pauseDownload(id);
                    if (result) env.showToast("Paused " + item.record.request.displayName);
                    break;
                case DownloadState::Paused:
                case DownloadState::Failed:
                    result = env.commands.resumeDownload(id);
                    if (result) env.showToast("Continuing " + item.record.request.displayName);
                    break;
                case DownloadState::Completed:
                    env.showToast("Downloaded and checked. Installing comes in a later version.");
                    break;
            }
            if (!result) env.showToast(result.error().message, ToastKind::Error);
            break;
        }
        case Action::Tertiary:
            removeId_ = id;
            return NavRequest::push(std::make_unique<ConfirmScreen>(
                "Remove this download?",
                std::vector<std::string>{item.record.request.displayName + " " + item.record.request.modVersion,
                                         "The downloaded file is deleted from the console. Your games are not affected."},
                "Remove", confirmRemove_.callback(), true));
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> DownloadsScreen::hints(const UiEnv& env) const {
    std::vector<ButtonHint> hints{{ButtonHint::Button::L1R1, "Switch tab"}};
    const auto& items = env.state.downloads.items;
    if (items.empty()) return hints;
    const int focus = std::clamp(list_.focus(), 0, static_cast<int>(items.size()) - 1);
    const DownloadState state = items[items.size() - 1 - static_cast<std::size_t>(focus)].record.state;
    hints.push_back({ButtonHint::Button::Square, "Remove"});
    switch (state) {
        case DownloadState::Queued:
        case DownloadState::Downloading:
        case DownloadState::Verifying: hints.push_back({ButtonHint::Button::Cross, "Pause"}); break;
        case DownloadState::Paused: hints.push_back({ButtonHint::Button::Cross, "Resume"}); break;
        case DownloadState::Failed: hints.push_back({ButtonHint::Button::Cross, "Try again"}); break;
        case DownloadState::Completed: break;
    }
    return hints;
}

bool DownloadsScreen::animating(const UiEnv& env) const {
    for (const auto& item : env.state.downloads.items) {
        if (item.record.state == DownloadState::Downloading || item.record.state == DownloadState::Verifying) return true;
    }
    return false;
}

void DownloadsScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const DownloadsView& view = env.state.downloads;
    canvas.drawText("Downloads", {content.x, content.y, 700, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    if (view.freeBytes) {
        canvas.drawText(strings::concat("Free space ", strings::formatBytes(*view.freeBytes), "   (",
                                        strings::formatBytes(view.reserveBytes), " is always kept free)"),
                        {content.x + 700, content.y, content.w - 700, 60},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});
    }
    const Rect listArea{content.x, content.y + 80, content.w, content.h - 80};

    const auto& report = env.state.systemCheck.report;
    int top = listArea.y;
    if (report && report->features.downloading.state != app::FeatureState::Available) {
        canvas.drawText("Downloading is not available: " + report->features.downloading.reason,
                        {listArea.x, top, listArea.w, 40},
                        TextStyle{FontRole::Caption, theme::kWarning, TextAlign::Left, false});
        top += 50;
    }
    const auto& items = view.items;
    if (items.empty()) {
        draw::messagePanel(canvas, {listArea.x, top, listArea.w, 300}, "Nothing downloaded yet",
                           "Find a mod in Discover and choose Download. Files are checked with SHA-256 before they "
                           "are kept, and nothing is installed.",
                           theme::kNeutral);
        return;
    }
    list_.setCount(static_cast<int>(items.size()));
    list_.setVisibleRows(kVisibleRows);
    constexpr int kRowHeight = 124;
    constexpr int kRowGap = 11;
    for (int i = list_.firstVisible(); i < std::min(list_.count(), list_.firstVisible() + kVisibleRows); ++i) {
        const DownloadInfo& item = items[items.size() - 1 - static_cast<std::size_t>(i)];
        const auto& record = item.record;
        const Rect box{listArea.x, top + (i - list_.firstVisible()) * (kRowHeight + kRowGap), listArea.w, kRowHeight};
        const bool focused = i == list_.focus();
        canvas.fillRoundedRect(box, 14, focused ? theme::kPanelRaised : theme::kPanel);
        if (focused) draw::focusRing(canvas, box, 14);

        const char* label = stateLabel(record.state);
        const int badgeWidth = canvas.measureText(label, FontRole::Small, true).w + 28;
        draw::badge(canvas, box.right() - badgeWidth - 24, box.y + 14, label, stateColor(record.state));
        std::string name = record.request.displayName;
        if (!record.request.modVersion.empty()) name += "  v" + record.request.modVersion;
        canvas.drawText(name, {box.x + 28, box.y + 8, box.w - badgeWidth - 80, 46},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, true});
        std::string context = record.request.gameTitleId;
        if (!record.request.compatibility.empty()) {
            context += (context.empty() ? "" : "   ") + record.request.compatibility;
        }
        canvas.drawText(context, {box.x + 28, box.y + 52, 520, 32},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Left, false});
        canvas.drawText(statusLine(item), {box.x + 560, box.y + 52, box.w - 560 - 24, 32},
                        TextStyle{FontRole::Small,
                                  record.state == DownloadState::Failed ? theme::kError : theme::kTextSecondary,
                                  TextAlign::Left, false});
        draw::progressBar(canvas, {box.x + 28, box.y + 94, box.w - 56, 14}, fraction(item), stateColor(record.state));
    }
    if (list_.count() > kVisibleRows) {
        canvas.drawText(strings::concat(list_.focus() + 1, " of ", list_.count()), {content.x, content.y + 40, content.w, 36},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Right, false});
    }
}

// ---------------------------------------------------------------- ConfirmScreen

ConfirmScreen::ConfirmScreen(std::string heading, std::vector<std::string> lines, std::string confirmLabel,
                             std::function<void(bool)> done, bool dangerous)
    : heading_(std::move(heading)),
      lines_(std::move(lines)),
      confirmLabel_(std::move(confirmLabel)),
      done_(std::move(done)),
      dangerous_(dangerous) {
    buttons_.setCount(2);
    buttons_.setVisibleRows(2);
    buttons_.setFocus(1);  // Cancel first
}

ConfirmScreen::~ConfirmScreen() { finish(false); }

void ConfirmScreen::finish(bool confirmed) {
    if (!done_) return;
    auto done = std::move(done_);
    done_ = nullptr;
    done(confirmed);
}

NavRequest ConfirmScreen::handle(Action action, UiEnv& /*env*/) {
    if (action == Action::Back) {
        finish(false);
        return NavRequest::pop();
    }
    if (buttons_.handle(action)) {
        return NavRequest::none();
    }
    if (action == Action::Confirm) {
        finish(buttons_.focus() == 0);
        return NavRequest::pop();
    }
    return NavRequest::none();
}

std::vector<ButtonHint> ConfirmScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::Circle, "Cancel"}, {ButtonHint::Button::Cross, "Select"}};
}

void ConfirmScreen::render(ICanvas& canvas, UiEnv& /*env*/) {
    const Rect box{(theme::kScreenWidth - 1200) / 2, 180, 1200, 640};
    draw::panel(canvas, box, true);
    canvas.fillRoundedRect({box.x, box.y, box.w, 10}, 5, dangerous_ ? theme::kWarning : theme::kAccent);
    canvas.drawText(heading_, {box.x + 60, box.y + 40, box.w - 120, 64},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
    int y = box.y + 130;
    for (const auto& line : lines_) {
        const int used = drawWrappedText(canvas, line, {box.x + 60, y, box.w - 120, 140},
                                         TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false}, 44, 3);
        y += used * 44 + 20;
        if (y > box.y + 420) break;
    }
    const std::array<std::string, 2> labels{confirmLabel_, "Cancel"};
    for (int i = 0; i < 2; ++i) {
        const Rect button{box.x + 60 + i * 540, box.bottom() - 120, 480, 76};
        draw::button(canvas, button, labels[static_cast<std::size_t>(i)], buttons_.focus() == i);
    }
}

}  // namespace akeno::ui
