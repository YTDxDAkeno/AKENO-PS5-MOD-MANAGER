// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Strings.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

namespace {

constexpr int kCardWidth = 320;
constexpr int kCardHeight = 330;
constexpr int kCardGapX = 40;
constexpr int kCardGapY = 32;
constexpr int kGridTop = 80;  // below the heading line

const char* sortLabel(database::LibrarySort sort) {
    switch (sort) {
        case database::LibrarySort::Name: return "Name";
        case database::LibrarySort::TitleId: return "Title ID";
        case database::LibrarySort::RecentlyPlayed: return "Recently played";
    }
    return "Name";
}

database::LibrarySort nextSort(database::LibrarySort sort) {
    switch (sort) {
        case database::LibrarySort::Name: return database::LibrarySort::TitleId;
        case database::LibrarySort::TitleId: return database::LibrarySort::RecentlyPlayed;
        case database::LibrarySort::RecentlyPlayed: return database::LibrarySort::Name;
    }
    return database::LibrarySort::Name;
}

void drawGameArt(ICanvas& canvas, UiEnv& env, const games::GameInfo& game, const Rect& rect, int size) {
    const std::string key = env.commands.gameIconKey(game, size);
    if (key.empty() || !canvas.drawImage(key, rect)) {
        draw::placeholderArt(canvas, rect, game.name);
    }
}

void drawMessage(ICanvas& canvas, const Rect& content, std::string_view heading, std::string_view body,
                 Color accent) {
    const Rect box{content.x, content.y + kGridTop, content.w, 300};
    draw::panel(canvas, box);
    canvas.fillRoundedRect({box.x, box.y, 10, box.h}, 5, accent);
    canvas.drawText(heading, {box.x + 50, box.y + 40, box.w - 100, 56},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    drawWrappedText(canvas, body, {box.x + 50, box.y + 120, box.w - 100, 160},
                    TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false}, 46, 3);
}

const games::GameInfo* findGame(const AppViewState& state, const std::string& titleId) {
    for (const auto& game : state.library.games) {
        if (game.titleId == titleId) return &game;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- GameLibraryScreen

NavRequest GameLibraryScreen::handle(Action action, UiEnv& env) {
    const auto& games = env.state.library.games;
    grid_.setColumns(kColumns);
    grid_.setVisibleRows(kVisibleRows);
    grid_.setCount(static_cast<int>(games.size()));
    if (grid_.handle(action)) {
        return NavRequest::none();
    }
    switch (action) {
        case Action::Confirm:
            if (!games.empty()) {
                return NavRequest::push(
                    std::make_unique<GameDetailScreen>(games[static_cast<std::size_t>(grid_.focus())].titleId));
            }
            break;
        case Action::Options: {
            const auto& report = env.state.systemCheck.report;
            if (report && report->features.gameLibrary.state != app::FeatureState::Available) {
                // ShadowMountPlus was not usable: check again; the library refreshes on success.
                env.commands.runSystemCheck();
                env.showToast("Checking for ShadowMountPlus again...");
            } else {
                env.commands.refreshLibrary();
                env.showToast("Refreshing the game list...");
            }
            break;
        }
        case Action::Secondary: {
            database::Settings settings = env.state.settings;
            settings.librarySort = nextSort(settings.librarySort);
            auto saved = env.commands.saveSettings(settings);
            env.showToast(saved ? strings::concat("Sorted by ", sortLabel(settings.librarySort))
                                : "Could not save: " + saved.error().message,
                          saved ? ToastKind::Info : ToastKind::Error);
            break;
        }
        case Action::Tertiary: {
            database::Settings settings = env.state.settings;
            settings.showPs4Games = !settings.showPs4Games;
            auto saved = env.commands.saveSettings(settings);
            env.showToast(saved ? (settings.showPs4Games ? "PS4 games shown" : "PS4 games hidden")
                                : "Could not save: " + saved.error().message,
                          saved ? ToastKind::Info : ToastKind::Error);
            break;
        }
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> GameLibraryScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::Square, "PS4 games"},
            {ButtonHint::Button::Triangle, "Sort"},
            {ButtonHint::Button::Options, "Refresh"},
            {ButtonHint::Button::Cross, "Details"}};
}

void GameLibraryScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const auto& library = env.state.library;
    const auto& games = library.games;
    grid_.setColumns(kColumns);
    grid_.setVisibleRows(kVisibleRows);
    grid_.setCount(static_cast<int>(games.size()));

    std::string heading = "Installed Games";
    if (library.everLoaded) heading += strings::concat(" (", games.size(), ")");
    canvas.drawText(heading, {content.x, content.y, 900, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    std::string filters = strings::concat("Sort: ", sortLabel(env.state.settings.librarySort),
                                          "   PS4 games: ", env.state.settings.showPs4Games ? "shown" : "hidden");
    canvas.drawText(filters, {content.x + 900, content.y, content.w - 900, 60},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});
    if (library.loading) {
        draw::spinner(canvas, content.x + 880, content.y + 30, env.time);
    }

    // The library depends on ShadowMountPlus; explain clearly when it is not available.
    const auto& report = env.state.systemCheck.report;
    if (report && report->features.gameLibrary.state != app::FeatureState::Available && games.empty()) {
        std::string reason = report->features.gameLibrary.reason;
        if (!reason.empty() && reason.back() != '.') reason += '.';
        drawMessage(canvas, content, "The game library is not available",
                    reason + " Start ShadowMountPlus (version 1.7 or newer), then press OPTIONS to try again.",
                    theme::kError);
        return;
    }
    if (!library.everLoaded) {
        if (library.error) {
            drawMessage(canvas, content, "Could not read your games", library.error->message, theme::kError);
        } else {
            draw::spinner(canvas, content.x + content.w / 2, content.y + 300, env.time);
            canvas.drawText("Asking ShadowMountPlus for your games...", {content.x, content.y + 360, content.w, 50},
                            TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Center, false});
        }
        return;
    }
    if (library.error) {
        canvas.drawText("Last refresh failed: " + library.error->message, {content.x, content.y + 44, content.w, 36},
                        TextStyle{FontRole::Small, theme::kWarning, TextAlign::Left, false});
    }
    if (games.empty()) {
        drawMessage(canvas, content, "No games found",
                    library.totalGames > 0
                        ? "All games are hidden by the current filters. Press SQUARE to show PS4 games, or enable "
                          "homebrew titles in Settings."
                        : "ShadowMountPlus did not report any installed games.",
                    theme::kNeutral);
        return;
    }

    const int firstIndex = grid_.firstVisibleRow() * kColumns;
    const int lastIndex = std::min(static_cast<int>(games.size()), firstIndex + kColumns * kVisibleRows);
    for (int index = firstIndex; index < lastIndex; ++index) {
        const auto& game = games[static_cast<std::size_t>(index)];
        const int slot = index - firstIndex;
        const int col = slot % kColumns;
        const int row = slot / kColumns;
        const Rect card{content.x + col * (kCardWidth + kCardGapX), content.y + kGridTop + row * (kCardHeight + kCardGapY),
                        kCardWidth, kCardHeight};
        const bool focused = index == grid_.focus();
        canvas.fillRoundedRect(card, 16, focused ? theme::kPanelRaised : theme::kPanel);
        if (focused) draw::focusRing(canvas, card, 16);
        const Rect art{card.x + (card.w - kIconSize) / 2, card.y + 14, kIconSize, kIconSize};
        drawGameArt(canvas, env, game, art, kIconSize);
        canvas.drawText(game.name, {card.x + 16, art.bottom() + 4, card.w - 32, 40},
                        TextStyle{FontRole::Caption, theme::kTextPrimary, TextAlign::Center, true});
        canvas.drawText(game.titleId, {card.x + 16, art.bottom() + 42, card.w - 32, 28},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Center, false});
        canvas.drawText("Version " + game.displayVersion(), {card.x + 16, art.bottom() + 68, card.w - 32, 28},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Center, false});
        canvas.drawText(strings::concat(game.installedMods, game.installedMods == 1 ? " installed mod" : " installed mods"),
                        {card.x + 16, art.bottom() + 94, card.w - 32, 28},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Center, false});
        if (game.previousVersion) {
            draw::badge(canvas, card.x + 12, card.y + 12, "UPDATED", theme::kWarning);
        }
    }
    if (grid_.rowCount() > kVisibleRows) {
        canvas.drawText(strings::concat("Row ", grid_.focus() / kColumns + 1, " of ", grid_.rowCount()),
                        {content.x, content.bottom() - 34, content.w, 34},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Right, false});
    }
}

// ---------------------------------------------------------------- GameDetailScreen

namespace {

enum class DetailAction { BrowseMods, Vanilla, ExportDiagnostics, Back };
constexpr std::array<DetailAction, 4> kDetailActions{DetailAction::BrowseMods, DetailAction::Vanilla,
                                                      DetailAction::ExportDiagnostics, DetailAction::Back};

}  // namespace

void GameDetailScreen::update(UiEnv& env) {
    bool confirmed = false;
    if (confirmVanilla_.take(confirmed) && confirmed) {
        env.commands.setGameVanilla(titleId_);
    }
    // The catalogue tells whether this game has mods; load it once in the background.
    const CatalogView& catalog = env.state.catalog;
    if (!catalogRequested_ && catalog.configured && !catalog.loaded && !catalog.loading) {
        catalogRequested_ = true;
        env.commands.loadCatalogGames(false);
    }
}

NavRequest GameDetailScreen::handle(Action action, UiEnv& env) {
    actions_.setCount(static_cast<int>(kDetailActions.size()));
    actions_.setVisibleRows(static_cast<int>(kDetailActions.size()));
    if (action == Action::Back) {
        return NavRequest::pop();
    }
    if (actions_.handle(action)) {
        return NavRequest::none();
    }
    if (action == Action::Confirm) {
        switch (kDetailActions[static_cast<std::size_t>(actions_.focus())]) {
            case DetailAction::BrowseMods: {
                const CatalogView& catalog = env.state.catalog;
                const games::GameInfo* game = findGame(env.state, titleId_);
                const providers::ProviderGame* entry = catalog.findByTitleId(titleId_);
                if (game != nullptr && entry != nullptr) {
                    return NavRequest::push(std::make_unique<ModBrowserScreen>(
                        entry->providerGameId, entry->name, providers::GameContext{game->titleId, game->version}));
                }
                if (!catalog.configured) {
                    env.showToast("The mod catalogue address is not valid. Check Settings.", ToastKind::Error);
                } else if (catalog.loading) {
                    env.showToast("Still loading the Akeno Catalogue...");
                } else if (!catalog.loaded) {
                    env.commands.loadCatalogGames(true);
                    env.showToast("The catalogue could not be loaded. Trying again...", ToastKind::Warning);
                } else {
                    env.showToast("The Akeno Catalogue has no mods for this game yet.");
                }
                break;
            }
            case DetailAction::Vanilla: {
                const InstalledModsSummary mods = env.commands.installedMods(titleId_);
                if (!mods.overlayActive && mods.enabled == 0) {
                    env.showToast("No mods are active for this game: it already runs unmodified.");
                    break;
                }
                std::vector<std::string> lines{
                    "Akeno removes its overlay for this game and turns every mod off. The game starts unmodified "
                    "the next time.",
                    "The mods stay stored and can be installed again from Downloads.",
                };
                return NavRequest::push(std::make_unique<ConfirmScreen>("Switch to Vanilla?", std::move(lines),
                                                                        "Vanilla", confirmVanilla_.callback(), false));
            }
            case DetailAction::ExportDiagnostics:
                if (env.state.diagnostics.running) env.commands.cancelDiagnostics();
                else env.commands.exportDiagnostics(titleId_);
                break;
            case DetailAction::Back:
                return NavRequest::pop();
        }
    }
    return NavRequest::none();
}

std::vector<ButtonHint> GameDetailScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::Circle, "Back"}, {ButtonHint::Button::Cross, "Select"}};
}

void GameDetailScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const games::GameInfo* game = findGame(env.state, titleId_);
    if (game == nullptr) {
        drawMessage(canvas, content, "Game not found",
                    "ShadowMountPlus no longer reports " + titleId_ + ". It may have been removed or its drive "
                    "disconnected. Press CIRCLE to go back.",
                    theme::kNeutral);
        return;
    }
    const Rect art{content.x, content.y, kIconSize, kIconSize};
    drawGameArt(canvas, env, *game, art, kIconSize);

    actions_.setCount(static_cast<int>(kDetailActions.size()));
    const CatalogView& catalog = env.state.catalog;
    const providers::ProviderGame* entry = catalog.findByTitleId(titleId_);
    std::string browseLabel = "Browse mods";
    if (entry != nullptr) browseLabel = strings::concat("Browse mods (", entry->modCount, ")");
    const InstalledModsSummary mods = env.commands.installedMods(titleId_);
    const bool modsActive = mods.overlayActive || mods.enabled > 0;
    const std::array<std::string, 4> labels{browseLabel, modsActive ? "Vanilla (mods off)" : "Vanilla",
        env.state.diagnostics.running ? "Cancel export" : "Export Diagnostics", "Back"};
    int buttonY = art.bottom() + 30;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const Rect row{content.x, buttonY + i * 84, kIconSize, 70};
        const DetailAction kind = kDetailActions[static_cast<std::size_t>(i)];
        const bool enabled = kind == DetailAction::Back || kind == DetailAction::ExportDiagnostics || (kind == DetailAction::BrowseMods && entry != nullptr) ||
                             (kind == DetailAction::Vanilla && modsActive);
        draw::button(canvas, row, labels[static_cast<std::size_t>(i)], actions_.focus() == i, enabled);
    }
    std::string note = strings::concat(mods.stored, " mods stored, none active");
    if (mods.error) {
        note = "The list of installed mods cannot be read";
    } else if (modsActive) {
        note = strings::concat(mods.enabled, " of ", mods.stored, " mods on",
                               mods.overlayActive ? ", overlay active" : "");
    }
    if (entry == nullptr) {
        if (catalog.loaded) {
            note = "No mods in the Akeno Catalogue yet";
        } else if (!catalog.configured || (catalog.error && !catalog.loading)) {
            note = "The Akeno Catalogue is unavailable";
        } else {
            note = "Checking the Akeno Catalogue...";
        }
    }
    canvas.drawText(note, {content.x, buttonY + 4 * 84, kIconSize, 36},
                    TextStyle{FontRole::Small, theme::kTextDisabled, TextAlign::Left, false});

    const int infoX = content.x + kIconSize + 60;
    const int infoW = content.right() - infoX;
    canvas.drawText(game->name, {infoX, content.y, infoW, 70},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});

    std::vector<std::pair<std::string, std::string>> rows{
        {"Title ID", game->titleId},
        {"Version", game->displayVersion() +
                        (game->previousVersion ? "  (previously " + *game->previousVersion + ")" : std::string())},
        {"Content ID", game->contentId.empty() ? "unknown" : game->contentId},
        {"Platform", game->platform == games::Platform::Ps5   ? "PS5"
                     : game->platform == games::Platform::Ps4 ? "PS4"
                                                              : "unknown"},
        {"Source", std::string(games::displayName(game->sourceType))},
        {"Install path", game->installPath.empty() ? "-" : game->installPath},
        {"Runtime path", game->runtimePath.empty() ? "-" : game->runtimePath},
        {"Mounted now", game->mounted ? "yes" : "no"},
        {"Installed mods", strings::concat(mods.stored, " (", mods.enabled, " on)")},
    };
    if (game->sizeBytes) rows.insert(rows.begin() + 5, {"Size", strings::formatBytes(*game->sizeBytes)});
    int y = content.y + 96;
    for (const auto& [label, value] : rows) {
        canvas.drawText(label, {infoX, y, 300, 44},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
        canvas.drawText(value, {infoX + 300, y, infoW - 300, 44},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, false});
        y += 54;
    }
    drawWrappedText(canvas,
                    env.state.diagnostics.progress.empty()
                        ? "Export Diagnostics reads stored mods, the overlay and accessible original folder files. "
                          "It does not activate mods or launch the game. No game assets are exported."
                        : env.state.diagnostics.progress + " " + env.state.diagnostics.lastExport,
                    {infoX, y + 16, infoW, 120},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false}, 38, 3);
}

}  // namespace akeno::ui
