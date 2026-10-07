// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3 screens: the Downloads tab and the confirmation dialog.
#include <algorithm>
#include <array>
#include <cmath>

#include "akeno/core/Strings.hpp"
#include "akeno/mods/ModCheck.hpp"
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
            return strings::concat(total, ", SHA-256 checked. Press X to check what is inside.");
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
                    return NavRequest::push(std::make_unique<ModCheckScreen>(id));
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
        case DownloadState::Completed: hints.push_back({ButtonHint::Button::Cross, "Check contents"}); break;
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

// ---------------------------------------------------------------- ModCheckScreen

namespace {

Color findingColor(mods::FindingLevel level) {
    switch (level) {
        case mods::FindingLevel::Info: return theme::kNeutral;
        case mods::FindingLevel::Warning: return theme::kWarning;
        case mods::FindingLevel::Blocker: return theme::kError;
    }
    return theme::kNeutral;
}

std::vector<draw::DocLine> buildCheckDocument(ICanvas& canvas, const mods::ModCheckReport& report, int width) {
    using draw::addHeadingLine;
    using draw::addWrappedLines;
    std::vector<draw::DocLine> lines;
    const mods::ModAnalysis& a = report.analysis;

    addHeadingLine(lines, "Findings");
    if (a.findings.empty()) {
        addWrappedLines(canvas, lines, "Nothing unusual: only game data, no programs or scripts.", width,
                        FontRole::Caption, theme::kTextPrimary, false, theme::kOk);
    }
    for (const auto& finding : a.findings) {
        std::string text = finding.message;
        if (!finding.path.empty()) text += "  (" + finding.path + ")";
        addWrappedLines(canvas, lines, text, width, FontRole::Caption,
                        finding.level == mods::FindingLevel::Info ? theme::kTextSecondary : theme::kTextPrimary, false,
                        findingColor(finding.level));
    }

    addHeadingLine(lines, "Conflicts with other checked mods of this game");
    if (report.conflicts.empty()) {
        addWrappedLines(canvas, lines, "None. No other checked mod writes the same files.", width, FontRole::Caption,
                        theme::kTextSecondary);
    }
    for (const auto& conflict : report.conflicts) {
        addWrappedLines(canvas, lines,
                        strings::concat(conflict.otherName, ": ", conflict.count,
                                        conflict.count == 1 ? " file in common" : " files in common"),
                        width, FontRole::Caption, theme::kTextPrimary, false, theme::kWarning);
        for (const auto& path : conflict.paths) {
            addWrappedLines(canvas, lines, "    " + path, width, FontRole::Small, theme::kTextSecondary);
        }
    }

    addHeadingLine(lines, "Install plan (dry run: nothing is changed)");
    if (!report.plan) {
        addWrappedLines(canvas, lines, "This download is not linked to an installed game, so there is no plan.", width,
                        FontRole::Caption, theme::kTextSecondary);
    } else {
        int number = 1;
        for (const auto& step : report.plan->steps) {
            addWrappedLines(canvas, lines, strings::concat(number++, ". ", step.title), width, FontRole::Caption,
                            theme::kTextPrimary, true);
            addWrappedLines(canvas, lines, step.detail, width, FontRole::Small, theme::kTextSecondary);
        }
        addWrappedLines(canvas, lines, "Not carried out: " + report.plan->notExecutableReason, width, FontRole::Caption,
                        theme::kWarning);
    }

    addHeadingLine(lines, strings::concat("Files (", a.files.size(), ")"));
    std::size_t shown = 0;
    for (const auto& file : a.files) {
        if (shown++ == 150) {
            addWrappedLines(canvas, lines, strings::concat("... and ", a.files.size() - 150, " more"), width,
                            FontRole::Small, theme::kTextSecondary);
            break;
        }
        std::string text = file.archivePath + "  " + strings::formatBytes(file.size) + "  " + std::string(toString(file.kind));
        text += file.installPath.empty() ? std::string("  (not installed)") : "  -> " + file.installPath;
        const bool code = file.kind == mods::FileKind::WindowsCode || file.kind == mods::FileKind::NativeCode;
        addWrappedLines(canvas, lines, text, width, FontRole::Small, code ? theme::kError : theme::kTextSecondary);
    }
    return lines;
}

}  // namespace

void ModCheckScreen::update(UiEnv& env) {
    if (!requested_) {
        requested_ = true;
        env.commands.checkDownload(downloadId_, false);
    }
}

NavRequest ModCheckScreen::handle(Action action, UiEnv& env) {
    const ModCheckView& view = env.state.check;
    const bool mine = view.downloadId == downloadId_;
    switch (action) {
        case Action::Back:
            if (mine && view.running) {
                env.commands.cancelCheck();
                env.showToast("Cancelling the check...");
                return NavRequest::none();
            }
            return NavRequest::pop();
        case Action::Secondary:
            if (!(mine && view.running)) {
                scroll_ = 0;
                env.commands.checkDownload(downloadId_, true);
            }
            break;
        case Action::PageDown:
        case Action::Down:
            scroll_ = std::min(maxScroll_, scroll_ + (action == Action::PageDown ? 8 : 1));
            break;
        case Action::PageUp:
        case Action::Up:
            scroll_ = std::max(0, scroll_ - (action == Action::PageUp ? 8 : 1));
            break;
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> ModCheckScreen::hints(const UiEnv& env) const {
    const ModCheckView& view = env.state.check;
    if (view.downloadId == downloadId_ && view.running) return {{ButtonHint::Button::Circle, "Cancel"}};
    return {{ButtonHint::Button::L2R2, "Scroll"}, {ButtonHint::Button::Triangle, "Check again"},
            {ButtonHint::Button::Circle, "Back"}};
}

void ModCheckScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const ModCheckView& view = env.state.check;
    const DownloadInfo* item = env.state.downloads.find(downloadId_);
    std::string name = item != nullptr ? item->record.request.displayName + "  v" + item->record.request.modVersion
                                       : std::string("Download");
    canvas.drawText("Check: " + name, {content.x, content.y, content.w - 400, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    if (view.downloadId != downloadId_) {
        if (view.running) {
            draw::messagePanel(canvas, {content.x, content.y + 90, content.w, 260}, "Another check is running",
                               "Wait until it has finished, then open this one again.", theme::kNeutral);
        }
        return;
    }
    if (view.running) {
        const Rect box{content.x, content.y + 100, content.w, 300};
        draw::panel(canvas, box);
        canvas.drawText(mods::describe(view.phase), {box.x + 50, box.y + 40, box.w - 100, 56},
                        TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
        draw::progressBar(canvas, {box.x + 50, box.y + 130, box.w - 100, 18}, view.progress, theme::kAccent);
        draw::spinner(canvas, box.right() - 80, box.y + 68, env.time);
        drawWrappedText(canvas,
                        "The archive is unpacked into Akeno's staging folder, never into a game. The staging folder is "
                        "deleted when the check is done.",
                        {box.x + 50, box.y + 180, box.w - 100, 100},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false}, 40, 2);
        return;
    }
    if (view.error) {
        const bool cancelled = view.error->code == ErrorCode::Cancelled;
        draw::messagePanel(canvas, {content.x, content.y + 90, content.w, 300},
                           cancelled ? "The check was cancelled" : "The check failed",
                           view.error->message + " Press TRIANGLE to check again.",
                           cancelled ? theme::kNeutral : theme::kError);
        return;
    }
    if (!view.report) return;
    const mods::ModCheckReport& report = *view.report;
    const mods::ModAnalysis& a = report.analysis;

    int x = content.x;
    const int badgeY = content.y + 70;
    x += draw::badge(canvas, x, badgeY, theme::badgeLabel(a.status), theme::badgeColor(a.status)) + 16;
    if (a.hasBlockers()) {
        draw::badge(canvas, x, badgeY, "BLOCKED", theme::kError);
    } else if (a.installable) {
        draw::badge(canvas, x, badgeY, "NO PROBLEMS FOUND", theme::kOk);
    } else {
        draw::badge(canvas, x, badgeY, "NOT INSTALLABLE", theme::kWarning);
    }
    std::string summary = strings::concat(a.installCount, a.installCount == 1 ? " file" : " files", " to install (",
                                          strings::formatBytes(a.installBytes), ") from ", report.archiveFiles,
                                          " in the archive");
    if (!a.engineHint.empty()) summary += "   " + a.engineHint;
    canvas.drawText(summary, {content.x, content.y + 120, content.w, 40},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
    canvas.drawText("Checked " + report.checkedAt.substr(0, 10) + "   " + report.titleId,
                    {content.x + content.w - 600, content.y, 600, 60},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});

    const Rect doc{content.x, content.y + 175, content.w, content.h - 175};
    draw::drawDocument(canvas, buildCheckDocument(canvas, report, doc.w - 30), doc, scroll_, maxScroll_);
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
