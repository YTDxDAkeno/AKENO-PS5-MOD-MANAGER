// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>

#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

namespace {

struct CheckSlot {
    app::CheckId id;
    const char* label;
};

constexpr std::array<CheckSlot, 8> kCheckSlots{{
    {app::CheckId::Firmware, "Firmware"},
    {app::CheckId::HomebrewEnvironment, "Homebrew environment"},
    {app::CheckId::ShadowMount, "ShadowMount"},
    {app::CheckId::ShadowMountApi, "ShadowMount API"},
    {app::CheckId::WritableStorage, "Writable data storage"},
    {app::CheckId::Networking, "Networking"},
    {app::CheckId::Database, "Database"},
    {app::CheckId::OverlayCapability, "Overlay capability"},
}};

const app::CheckResult* findResult(const SystemCheckView& view, app::CheckId id) {
    for (const auto& result : view.results) {
        if (result.id == id) return &result;
    }
    return nullptr;
}

void statusIcon(ICanvas& canvas, int cx, int cy, const app::CheckResult* result) {
    if (result == nullptr) {
        canvas.fillCircle(cx, cy, 18, theme::kPanelRaised);
        return;
    }
    Color color = theme::kNeutral;
    const char* glyph = "\xE2\x80\x93";  // en dash: skipped
    switch (result->status) {
        case app::CheckStatus::Ok: color = theme::kOk; glyph = "\xE2\x9C\x93"; break;
        case app::CheckStatus::Warning: color = theme::kWarning; glyph = "!"; break;
        case app::CheckStatus::Failed: color = theme::kError; glyph = "\xE2\x9C\x95"; break;
        case app::CheckStatus::Skipped: break;
    }
    canvas.fillCircle(cx, cy, 18, color);
    canvas.drawText(glyph, {cx - 18, cy - 18, 36, 36}, TextStyle{FontRole::Small, theme::kTextPrimary, TextAlign::Center, true});
}

// One row per check, in a fixed order so pending checks are visible too.
void drawCheckRows(ICanvas& canvas, UiEnv& env, int x, int y, int width, int rowHeight,
                   const std::vector<app::CheckId>& ids) {
    const auto& view = env.state.systemCheck;
    for (app::CheckId id : ids) {
        const char* label = "";
        for (const auto& slot : kCheckSlots) {
            if (slot.id == id) label = slot.label;
        }
        const app::CheckResult* result = findResult(view, id);
        statusIcon(canvas, x + 20, y + rowHeight / 2, result);
        canvas.drawText(label, {x + 60, y, 480, rowHeight},
                        TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false});
        std::string summary = result ? result->summary : (view.running ? "checking..." : "not checked");
        canvas.drawText(summary, {x + 560, y, width - 560, rowHeight},
                        TextStyle{FontRole::Body, result ? theme::kTextPrimary : theme::kTextDisabled, TextAlign::Left,
                                  false});
        y += rowHeight;
    }
}

std::vector<app::CheckId> allCheckIds() {
    std::vector<app::CheckId> ids;
    for (const auto& slot : kCheckSlots) ids.push_back(slot.id);
    return ids;
}

void drawFeatureSummary(ICanvas& canvas, const app::SystemReport& report, const Rect& box) {
    draw::panel(canvas, box);
    const bool safeMode = report.features.safeMode();
    canvas.fillRoundedRect({box.x, box.y, 12, box.h}, 6, safeMode ? theme::kWarning : theme::kOk);
    canvas.drawText(safeMode ? "SAFE MODE: ON" : "SAFE MODE: OFF", {box.x + 44, box.y + 20, 600, 56},
                    TextStyle{FontRole::Heading, safeMode ? theme::kWarning : theme::kOk, TextAlign::Left, true});
    int y = box.y + 90;
    const int columnWidth = (box.w - 88) / 2;
    int index = 0;
    for (const app::Feature* feature : report.features.all()) {
        const int col = index % 2;
        const int row = index / 2;
        Color color = feature->state == app::FeatureState::Available ? theme::kOk
                      : feature->state == app::FeatureState::Disabled ? theme::kError
                                                                      : theme::kNeutral;
        canvas.drawText(strings::concat(feature->name, ": ", app::toString(feature->state)),
                        {box.x + 44 + col * columnWidth, y + row * 44, columnWidth, 44},
                        TextStyle{FontRole::Caption, color, TextAlign::Left, true});
        ++index;
    }
    if (safeMode && !report.features.installation.reason.empty()) {
        drawWrappedText(canvas, "Reason: " + report.features.installation.reason,
                        {box.x + 44, y + 100, box.w - 88, 80},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false}, 38, 2);
    }
}

}  // namespace

// ---------------------------------------------------------------- SystemCheckScreen

NavRequest SystemCheckScreen::handle(Action action, UiEnv& env) {
    const bool running = env.state.systemCheck.running;
    if ((action == Action::Confirm || action == Action::Back) && !running) {
        return NavRequest::pop();
    }
    if (action == Action::Secondary && !running) {
        env.commands.runSystemCheck();
    }
    if (action == Action::Tertiary && !running) {
        auto exported = env.commands.exportDiagnostics();
        env.showToast(exported ? "Report written to " + exported.value() : "Export failed: " + exported.error().message,
                      exported ? ToastKind::Success : ToastKind::Error);
    }
    return NavRequest::none();
}

std::vector<ButtonHint> SystemCheckScreen::hints(const UiEnv& env) const {
    if (env.state.systemCheck.running) {
        return {};
    }
    return {{ButtonHint::Button::Square, "Export report"},
            {ButtonHint::Button::Triangle, "Check again"},
            {ButtonHint::Button::Cross, "Continue"}};
}

void SystemCheckScreen::render(ICanvas& canvas, UiEnv& env) {
    const int x = theme::kMargin;
    const int width = theme::kScreenWidth - 2 * theme::kMargin;
    canvas.drawText("AKENO PS5 MOD MANAGER", {x, 40, width, 44},
                    TextStyle{FontRole::Caption, theme::kAccent, TextAlign::Left, true});
    canvas.drawText("System Check", {x, 84, width, 72}, TextStyle{FontRole::Display, theme::kTextPrimary, TextAlign::Left, true});
    if (env.state.systemCheck.running) {
        draw::spinner(canvas, theme::kScreenWidth - theme::kMargin - 40, 120, env.time);
    }
    drawCheckRows(canvas, env, x, 190, width, 66, allCheckIds());
    if (env.state.systemCheck.report) {
        drawFeatureSummary(canvas, *env.state.systemCheck.report, {x, 730, width, 260});
    }
}

// ---------------------------------------------------------------- WizardScreen

bool WizardScreen::animating(const UiEnv& env) const {
    return env.state.systemCheck.running || env.state.library.loading;
}

NavRequest WizardScreen::handle(Action action, UiEnv& env) {
    if (action == Action::Back) {
        if (step_ > 0) --step_;
        return NavRequest::none();
    }
    if (action == Action::Secondary && (step_ == 1 || step_ == 2) && !env.state.systemCheck.running) {
        env.commands.runSystemCheck();
        return NavRequest::none();
    }
    if (action != Action::Confirm) {
        return NavRequest::none();
    }
    if ((step_ == 1 || step_ == 2) && env.state.systemCheck.running) {
        return NavRequest::none();  // wait for the check
    }
    if (step_ == 0 && !env.state.systemCheck.report && !env.state.systemCheck.running) {
        env.commands.runSystemCheck();
    }
    if (step_ == 2 && !libraryRequested_) {
        // Entering "Detect games".
        env.commands.refreshLibrary();
        libraryRequested_ = true;
    }
    if (step_ < kStepCount - 1) {
        ++step_;
        return NavRequest::none();
    }
    database::Settings settings = env.state.settings;
    settings.firstRunComplete = true;
    auto saved = env.commands.saveSettings(settings);
    if (!saved) {
        env.showToast("Could not save your progress: " + saved.error().message, ToastKind::Warning);
    }
    return NavRequest::pop();
}

std::vector<ButtonHint> WizardScreen::hints(const UiEnv& /*env*/) const {
    std::vector<ButtonHint> hints;
    if (step_ > 0) hints.push_back({ButtonHint::Button::Circle, "Back"});
    if (step_ == 1 || step_ == 2) hints.push_back({ButtonHint::Button::Triangle, "Check again"});
    hints.push_back({ButtonHint::Button::Cross, step_ == 0 ? "Start" : step_ == kStepCount - 1 ? "Finish" : "Next"});
    return hints;
}

void WizardScreen::render(ICanvas& canvas, UiEnv& env) {
    const int x = theme::kMargin + 80;
    const int width = theme::kScreenWidth - 2 * x;
    const TextStyle body{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false};
    static constexpr std::array<const char*, 5> kSteps{
        "Check homebrew environment", "Connect ShadowMount", "Detect games", "Choose mod providers",
        "Create Vanilla profiles"};

    if (step_ > 0 && step_ < kStepCount - 1) {
        canvas.drawText(strings::concat("Step ", step_, " of 5"), {x, 60, width, 44},
                        TextStyle{FontRole::Caption, theme::kAccent, TextAlign::Left, true});
        canvas.drawText(kSteps[static_cast<std::size_t>(step_ - 1)], {x, 104, width, 72},
                        TextStyle{FontRole::Display, theme::kTextPrimary, TextAlign::Left, true});
        draw::progressBar(canvas, {x, 196, width, 12}, step_ / 5.0, theme::kAccent);
    }

    switch (step_) {
        case 0: {
            canvas.drawText("Welcome to", {x, 120, width, 60}, TextStyle{FontRole::Heading, theme::kTextSecondary});
            canvas.drawText("Akeno PS5 Mod Manager", {x, 180, width, 90},
                            TextStyle{FontRole::Display, theme::kTextPrimary, TextAlign::Left, true});
            drawWrappedText(canvas,
                            "This guide checks your console environment and finds your installed games. This "
                            "version is an experimental alpha: it can browse your library, but it does not "
                            "download or install mods yet, and it never changes game files.",
                            {x, 300, width, 150}, body, 46, 3);
            for (std::size_t i = 0; i < kSteps.size(); ++i) {
                canvas.drawText(strings::concat("Step ", i + 1, "   ", kSteps[i]), {x, 500 + static_cast<int>(i) * 66, width, 60},
                                TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false});
            }
            break;
        }
        case 1:
            drawWrappedText(canvas, "Akeno looks at what this console can do. Nothing is enabled because of a "
                                    "firmware number; only detected capabilities count.",
                            {x, 250, width, 100}, body, 46, 2);
            drawCheckRows(canvas, env, x, 380, width, 76,
                          {app::CheckId::Firmware, app::CheckId::HomebrewEnvironment, app::CheckId::WritableStorage,
                           app::CheckId::Networking, app::CheckId::Database});
            break;
        case 2: {
            drawWrappedText(canvas, "Akeno reads your game library from ShadowMountPlus through its local API on "
                                    "this console (127.0.0.1). The API is never opened to the network.",
                            {x, 250, width, 100}, body, 46, 2);
            drawCheckRows(canvas, env, x, 380, width, 76, {app::CheckId::ShadowMount, app::CheckId::ShadowMountApi});
            const auto* api = findResult(env.state.systemCheck, app::CheckId::ShadowMountApi);
            if (api != nullptr && api->status != app::CheckStatus::Ok) {
                drawWrappedText(canvas,
                                "Load ShadowMountPlus 1.7 or newer with your payload loader, then press TRIANGLE to "
                                "check again. You can continue without it, but the game library will be empty.",
                                {x, 560, width, 140}, TextStyle{FontRole::Body, theme::kWarning, TextAlign::Left, false},
                                46, 3);
            }
            break;
        }
        case 3: {
            const auto& library = env.state.library;
            if (library.loading) {
                draw::spinner(canvas, x + 40, 300, env.time);
                canvas.drawText("Looking for games...", {x + 100, 270, width, 60}, body);
            } else if (library.error) {
                drawWrappedText(canvas, "Games could not be read: " + library.error->message, {x, 260, width, 140},
                                TextStyle{FontRole::Body, theme::kWarning, TextAlign::Left, false}, 46, 3);
            } else {
                canvas.drawText(strings::concat(library.games.size(), " games found"), {x, 260, width, 70},
                                TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
                int y = 360;
                for (std::size_t i = 0; i < library.games.size() && i < 7; ++i) {
                    const auto& game = library.games[i];
                    canvas.drawText(strings::concat(game.titleId, "   ", game.name, "   v", game.displayVersion()),
                                    {x, y, width, 56}, body);
                    y += 60;
                }
            }
            break;
        }
        case 4:
            drawWrappedText(canvas,
                            "Akeno Catalogue: a curated list of mods checked for PS5, hosted on GitHub. It is the "
                            "default source and can be browsed in the Discover tab.",
                            {x, 260, width, 100}, TextStyle{FontRole::Body, theme::kTextPrimary}, 46, 2);
            drawWrappedText(canvas,
                            "Nexus Mods and mod.io are planned for Phase 7, only through their official APIs and "
                            "rules. Akeno will never scrape websites or bypass download restrictions.",
                            {x, 400, width, 140}, body, 46, 3);
            drawWrappedText(canvas, "There is nothing else to choose in this version.", {x, 580, width, 60},
                            TextStyle{FontRole::Body, theme::kWarning}, 46, 1);
            break;
        case 5:
            drawWrappedText(canvas,
                            "Vanilla means no mods are active. It is a permanent profile for every game and always "
                            "restores the original game without reinstalling it.",
                            {x, 260, width, 100}, TextStyle{FontRole::Body, theme::kTextPrimary}, 46, 2);
            drawWrappedText(canvas,
                            "Akeno never writes to the original game files. Because this version cannot install "
                            "mods, every game is currently Vanilla.",
                            {x, 400, width, 140}, body, 46, 3);
            break;
        default: {
            canvas.drawText("Ready.", {x, 200, width, 90},
                            TextStyle{FontRole::Display, theme::kTextPrimary, TextAlign::Left, true});
            if (env.state.systemCheck.report) {
                drawFeatureSummary(canvas, *env.state.systemCheck.report, {x, 340, width, 280});
            }
            drawWrappedText(canvas, "You can run this guide again from Settings.", {x, 660, width, 60}, body, 46, 1);
            break;
        }
    }
}

// ---------------------------------------------------------------- RecoveryScreen

namespace {
enum class RecoveryAction { Clean, Later };

std::vector<RecoveryAction> recoveryActions(const AppViewState& state) {
    std::vector<RecoveryAction> actions;
    if (state.recovery && state.recovery->advice.canCleanStaging) actions.push_back(RecoveryAction::Clean);
    actions.push_back(RecoveryAction::Later);
    return actions;
}
}  // namespace

NavRequest RecoveryScreen::handle(Action action, UiEnv& env) {
    const auto actions = recoveryActions(env.state);
    actions_.setCount(static_cast<int>(actions.size()));
    actions_.setVisibleRows(static_cast<int>(actions.size()));
    if (actions_.handle(action)) {
        return NavRequest::none();
    }
    if (action == Action::Back) {
        env.commands.postponeRecovery();
        return NavRequest::pop();
    }
    if (action != Action::Confirm) {
        return NavRequest::none();
    }
    switch (actions[static_cast<std::size_t>(actions_.focus())]) {
        case RecoveryAction::Clean: {
            auto cleaned = env.commands.cleanInterruptedOperation();
            if (!cleaned) {
                env.showToast("Cleanup failed: " + cleaned.error().message, ToastKind::Error);
                return NavRequest::none();
            }
            env.showToast("Staging data removed.", ToastKind::Success);
            return NavRequest::pop();
        }
        case RecoveryAction::Later:
            env.commands.postponeRecovery();
            return NavRequest::pop();
    }
    return NavRequest::none();
}

std::vector<ButtonHint> RecoveryScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::Circle, "Decide later"}, {ButtonHint::Button::Cross, "Select"}};
}

void RecoveryScreen::render(ICanvas& canvas, UiEnv& env) {
    const int x = theme::kMargin + 80;
    const int width = theme::kScreenWidth - 2 * x;
    if (!env.state.recovery) {
        canvas.drawText("Nothing to recover.", {x, 200, width, 80}, TextStyle{FontRole::Title, theme::kTextPrimary});
        return;
    }
    const RecoveryView& recovery = *env.state.recovery;
    canvas.drawText(recovery.advice.headline, {x, 120, width, 80},
                    TextStyle{FontRole::Display, theme::kWarning, TextAlign::Left, true});
    drawWrappedText(canvas, recovery.advice.explanation, {x, 230, width, 100},
                    TextStyle{FontRole::Body, theme::kTextPrimary}, 46, 2);
    std::vector<std::pair<std::string, std::string>> rows{
        {"Operation", recovery.state.description.empty() ? recovery.state.kind : recovery.state.description},
        {"Last step", recovery.state.step.empty() ? "-" : recovery.state.step},
        {"Started", recovery.state.startedAt.empty() ? "unknown" : recovery.state.startedAt},
        {"Staging folders", std::to_string(recovery.state.stagingPaths.size())},
    };
    int y = 350;
    for (const auto& [label, value] : rows) {
        canvas.drawText(label, {x, y, 340, 50}, TextStyle{FontRole::Caption, theme::kTextSecondary});
        canvas.drawText(value, {x + 340, y, width - 340, 50}, TextStyle{FontRole::Body, theme::kTextPrimary});
        y += 56;
    }
    if (recovery.advice.needsOverlayRestore) {
        drawWrappedText(canvas,
                        "This version of Akeno does not manage mod overlays, so there is no overlay to restore. If "
                        "the operation came from a newer version of Akeno, open that version to finish recovery.",
                        {x, y + 10, width, 100}, TextStyle{FontRole::Caption, theme::kTextSecondary}, 40, 2);
        y += 110;
    }
    const auto actions = recoveryActions(env.state);
    actions_.setCount(static_cast<int>(actions.size()));
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        const Rect row{x, y + 30 + i * 92, 900, 76};
        draw::button(canvas, row,
                     actions[static_cast<std::size_t>(i)] == RecoveryAction::Clean ? "Clean up staging data"
                                                                                  : "Decide later",
                     actions_.focus() == i);
    }
}

// ---------------------------------------------------------------- setup

void setupScreens(ScreenHost& host, const AppViewState& state) {
    host.setTabRoot(Tab::Home, std::make_unique<HomeScreen>());
    host.setTabRoot(Tab::Games, std::make_unique<GameLibraryScreen>());
    host.setTabRoot(Tab::Discover, std::make_unique<DiscoverScreen>());
    host.setTabRoot(Tab::Downloads, std::make_unique<DownloadsScreen>());
    host.setTabRoot(Tab::InstalledMods,
                    std::make_unique<PlannedFeatureScreen>(
                        "Installed Mods", "Planned for Phases 5 and 6",
                        std::vector<std::string>{
                            "Mods will be applied through ShadowMountPlus overlays. Original game files are never "
                            "modified.",
                            "Enable, disable and reorder mods. Conflicts are shown before anything changes.",
                            "A permanent Vanilla profile always restores the unmodified game."}));
    host.setTabRoot(Tab::Updates,
                    std::make_unique<PlannedFeatureScreen>(
                        "Updates", "Planned for Phase 6",
                        std::vector<std::string>{
                            "Akeno will compare installed mod versions with their sources and offer updates.",
                            "Every update goes through the full verification and safety pipeline again."}));
    host.setTabRoot(Tab::Settings, std::make_unique<SettingsScreen>());
    host.setTabRoot(Tab::About, std::make_unique<AboutScreen>());

    // Flows are a stack: the last one pushed is shown first.
    if (state.settings.firstRunComplete) {
        host.pushFlow(std::make_unique<SystemCheckScreen>());
    } else {
        host.pushFlow(std::make_unique<WizardScreen>());
    }
    if (state.recovery) {
        host.pushFlow(std::make_unique<RecoveryScreen>());
    }
}

}  // namespace akeno::ui
