// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 2 screens: Discover (catalogue games), the mod list of a game, mod details and the
// screenshot viewer. Downloading and installing are not part of this version; the screens say
// so instead of offering actions that do nothing.
#include <algorithm>
#include <array>

#include "akeno/core/Strings.hpp"
#include "akeno/network/Url.hpp"
#include "akeno/ui/Screens.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

using providers::BrowseOrder;
using providers::CompatibilityCheck;
using providers::CompatibilityStatus;

namespace {

constexpr std::string_view kNotYetInstallable =
    "Downloading comes in the next version, installing after that. Nothing on your console was changed.";

const char* orderLabel(BrowseOrder order) {
    switch (order) {
        case BrowseOrder::Featured: return "Featured";
        case BrowseOrder::Popular: return "Popular";
        case BrowseOrder::Newest: return "Newest";
        case BrowseOrder::Verified: return "Best compatibility";
    }
    return "Featured";
}

BrowseOrder nextOrder(BrowseOrder order) {
    switch (order) {
        case BrowseOrder::Featured: return BrowseOrder::Newest;
        case BrowseOrder::Newest: return BrowseOrder::Verified;
        case BrowseOrder::Verified: return BrowseOrder::Popular;
        case BrowseOrder::Popular: return BrowseOrder::Featured;
    }
    return BrowseOrder::Featured;
}

Color markColor(CompatibilityCheck::Mark mark) {
    switch (mark) {
        case CompatibilityCheck::Mark::Pass: return theme::kOk;
        case CompatibilityCheck::Mark::Warn: return theme::kWarning;
        case CompatibilityCheck::Mark::Fail: return theme::kError;
        case CompatibilityCheck::Mark::Unknown: return theme::kNeutral;
    }
    return theme::kNeutral;
}

Color riskColor(std::string_view risk) {
    if (risk == "LOW") return theme::kOk;
    if (risk == "MEDIUM") return theme::kWarning;
    if (risk == "HIGH") return theme::kError;
    return theme::kNeutral;
}

int statusBadge(ICanvas& canvas, int x, int y, CompatibilityStatus status) {
    return draw::badge(canvas, x, y, theme::badgeLabel(status), theme::badgeColor(status));
}

// Width of a badge as draw::badge lays it out, for right alignment.
int badgeWidth(ICanvas& canvas, std::string_view label) { return canvas.measureText(label, FontRole::Small, true).w + 28; }

std::string hostOf(const std::string& url) {
    auto parsed = network::parseUrl(url);
    return parsed ? parsed->authority() : url;
}

const games::GameInfo* installedGame(const AppViewState& state, const providers::ProviderGame& game) {
    for (const auto& installed : state.library.games) {
        for (const auto& id : game.titleIds) {
            if (installed.titleId == id) return &installed;
        }
    }
    return nullptr;
}

struct DiscoverRow {
    const providers::ProviderGame* game;
    const games::GameInfo* installed;
};

// Installed games first, then by name.
std::vector<DiscoverRow> discoverRows(const AppViewState& state) {
    std::vector<DiscoverRow> rows;
    rows.reserve(state.catalog.games.size());
    for (const auto& game : state.catalog.games) {
        rows.push_back({&game, installedGame(state, game)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const DiscoverRow& a, const DiscoverRow& b) {
        if ((a.installed != nullptr) != (b.installed != nullptr)) return a.installed != nullptr;
        return strings::toLowerAscii(a.game->name) < strings::toLowerAscii(b.game->name);
    });
    return rows;
}

void loadingMessage(ICanvas& canvas, const Rect& area, std::string_view text, double time) {
    draw::spinner(canvas, area.x + area.w / 2, area.y + 160, time);
    canvas.drawText(text, {area.x, area.y + 220, area.w, 50},
                    TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Center, false});
}

bool sameQuery(const providers::SearchQuery& a, const providers::SearchQuery& b) {
    const bool sameGame = a.game.has_value() == b.game.has_value() &&
                          (!a.game || (a.game->titleId == b.game->titleId && a.game->version == b.game->version));
    return sameGame && a.providerGameId == b.providerGameId && a.text == b.text && a.order == b.order &&
           a.offset == b.offset && a.limit == b.limit;
}

}  // namespace

// ---------------------------------------------------------------- DiscoverScreen

void DiscoverScreen::update(UiEnv& env) {
    // Loads once per catalogue address; after a failure OPTIONS tries again.
    const CatalogView& catalog = env.state.catalog;
    if (catalog.configured && !catalog.loaded && !catalog.loading && requestedSource_ != catalog.source) {
        requestedSource_ = catalog.source;
        env.commands.loadCatalogGames(false);
    }
}

NavRequest DiscoverScreen::handle(Action action, UiEnv& env) {
    const auto rows = discoverRows(env.state);
    list_.setCount(static_cast<int>(rows.size()));
    list_.setVisibleRows(kVisibleRows);
    if (list_.handle(action)) {
        return NavRequest::none();
    }
    switch (action) {
        case Action::Confirm:
            if (!rows.empty()) {
                const DiscoverRow& row = rows[static_cast<std::size_t>(list_.focus())];
                std::optional<providers::GameContext> context;
                if (row.installed != nullptr) {
                    context = providers::GameContext{row.installed->titleId, row.installed->version};
                }
                return NavRequest::push(
                    std::make_unique<ModBrowserScreen>(row.game->providerGameId, row.game->name, context));
            }
            break;
        case Action::Options:
            if (!env.state.catalog.configured) {
                env.showToast("Set a valid catalogue address in Settings first.", ToastKind::Warning);
            } else {
                requestedSource_ = env.state.catalog.source;
                env.commands.loadCatalogGames(true);
                env.showToast("Refreshing the Akeno Catalogue...");
            }
            break;
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> DiscoverScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L1R1, "Switch tab"},
            {ButtonHint::Button::Options, "Refresh"},
            {ButtonHint::Button::Cross, "Show mods"}};
}

void DiscoverScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const CatalogView& catalog = env.state.catalog;
    canvas.drawText("Discover mods", {content.x, content.y, 900, 60},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    canvas.drawText("Source: Akeno Catalogue (" + hostOf(catalog.source) + ")", {content.x + 900, content.y, content.w - 900, 60},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});

    const Rect listArea{content.x, content.y + 80, 1100, content.h - 80};
    const Rect side{content.x + 1140, content.y + 80, content.w - 1140, 560};
    draw::panel(canvas, side);
    canvas.drawText("About the catalogue", {side.x + 30, side.y + 24, side.w - 60, 50},
                    TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, true});
    drawWrappedText(canvas,
                    "A curated list of mods checked for PS5. Every mod is labelled for the version of the game you "
                    "have installed. VERIFIED appears only when it was tested with exactly that version.",
                    {side.x + 30, side.y + 90, side.w - 60, 250},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false}, 38, 6);
    drawWrappedText(canvas,
                    "This version can browse mods. Downloading comes next; installing follows once it has been "
                    "tested on real consoles.",
                    {side.x + 30, side.y + 340, side.w - 60, 200},
                    TextStyle{FontRole::Caption, theme::kWarning, TextAlign::Left, false}, 38, 5);

    if (!catalog.configured) {
        std::string body = catalog.configurationError ? catalog.configurationError->message : std::string();
        if (!body.empty() && body.back() != '.') body += '.';
        draw::messagePanel(canvas, {listArea.x, listArea.y, listArea.w, 320}, "The catalogue address is not valid",
                           body + " Change it under Settings > Mod catalogue address.", theme::kError);
        return;
    }
    if (!catalog.loaded) {
        if (catalog.error && !catalog.loading) {
            draw::messagePanel(canvas, {listArea.x, listArea.y, listArea.w, 320}, "Could not load the Akeno Catalogue",
                               catalog.error->message + " Check the network connection, then press OPTIONS to try again.",
                               theme::kError);
        } else {
            loadingMessage(canvas, listArea, "Loading the Akeno Catalogue...", env.time);
        }
        return;
    }
    if (catalog.loading) {
        draw::spinner(canvas, content.x + 880, content.y + 30, env.time);
    }
    const auto rows = discoverRows(env.state);
    if (rows.empty()) {
        draw::messagePanel(canvas, {listArea.x, listArea.y, listArea.w, 320}, "The catalogue is empty",
                           "No games have mods in this catalogue yet.", theme::kNeutral);
        return;
    }
    list_.setCount(static_cast<int>(rows.size()));
    list_.setVisibleRows(kVisibleRows);
    constexpr int kRowHeight = 120;
    constexpr int kRowGap = 14;
    for (int i = list_.firstVisible(); i < std::min(list_.count(), list_.firstVisible() + kVisibleRows); ++i) {
        const DiscoverRow& row = rows[static_cast<std::size_t>(i)];
        const Rect box{listArea.x, listArea.y + (i - list_.firstVisible()) * (kRowHeight + kRowGap), listArea.w, kRowHeight};
        const bool focused = i == list_.focus();
        canvas.fillRoundedRect(box, 14, focused ? theme::kPanelRaised : theme::kPanel);
        if (focused) draw::focusRing(canvas, box, 14);
        const Rect art{box.x + 12, box.y + 12, 96, 96};
        std::string key;
        if (row.installed != nullptr) key = env.commands.gameIconKey(*row.installed, 96);
        if (key.empty() || !canvas.drawImage(key, art)) draw::placeholderArt(canvas, art, row.game->name);
        const int textX = art.right() + 28;
        int badgeSpace = 0;
        if (row.installed != nullptr) {
            badgeSpace = badgeWidth(canvas, "INSTALLED") + 24;
            draw::badge(canvas, box.right() - badgeSpace, box.y + 16, "INSTALLED", theme::kOk);
        }
        canvas.drawText(row.game->name, {textX, box.y + 10, box.right() - textX - badgeSpace - 10, 52},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, true});
        std::string detail = strings::concat(row.game->modCount, row.game->modCount == 1 ? " mod" : " mods");
        if (row.installed != nullptr) {
            detail += "   Installed version " + row.installed->displayVersion();
        } else {
            detail += "   Not installed";
        }
        if (!row.game->titleIds.empty()) detail += "   " + row.game->titleIds.front();
        canvas.drawText(detail, {textX, box.y + 64, box.right() - textX - 20, 40},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
    }
    if (list_.count() > kVisibleRows) {
        canvas.drawText(strings::concat(list_.focus() + 1, " of ", list_.count()),
                        {side.x, side.bottom() + 20, side.w, 40},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Right, false});
    }
}

// ---------------------------------------------------------------- ModBrowserScreen

ModBrowserScreen::ModBrowserScreen(std::string providerGameId, std::string gameName,
                                   std::optional<providers::GameContext> game)
    : gameName_(std::move(gameName)) {
    query_.providerGameId = std::move(providerGameId);
    query_.game = std::move(game);
    query_.limit = 100;
}

void ModBrowserScreen::update(UiEnv& env) {
    std::optional<std::string> text;
    if (search_.take(text) && text) {
        if (*text != query_.text) {
            query_.text = *text;
            query_.offset = 0;
            list_.setFocus(0);
        }
    }
    // (Re)load whenever the shared list shows something else, e.g. after another game's list.
    if (!sameQuery(env.state.modList.query, query_)) {
        env.commands.loadModList(query_);
    }
}

NavRequest ModBrowserScreen::handle(Action action, UiEnv& env) {
    const ModListView& view = env.state.modList;
    const bool current = sameQuery(view.query, query_);
    const auto* mods = current && view.page ? &view.page->mods : nullptr;
    list_.setCount(mods != nullptr ? static_cast<int>(mods->size()) : 0);
    list_.setVisibleRows(kVisibleRows);
    if (action == Action::Back) {
        return NavRequest::pop();
    }
    if (list_.handle(action)) {
        return NavRequest::none();
    }
    switch (action) {
        case Action::Confirm:
            if (mods != nullptr && !mods->empty()) {
                const auto& mod = (*mods)[static_cast<std::size_t>(list_.focus())];
                return NavRequest::push(std::make_unique<ModDetailScreen>(mod.ref, gameName_, query_.game));
            }
            break;
        case Action::Secondary:
            query_.order = nextOrder(query_.order);
            query_.offset = 0;
            list_.setFocus(0);
            env.commands.loadModList(query_);
            env.showToast(strings::concat("Order: ", orderLabel(query_.order)));
            break;
        case Action::Tertiary:
            env.commands.requestTextInput("Search mods for " + gameName_, query_.text, search_.callback());
            break;
        case Action::Options:
            if (!query_.text.empty()) {
                query_.text.clear();
                query_.offset = 0;
                list_.setFocus(0);
                env.commands.loadModList(query_);
                env.showToast("Search cleared");
            }
            break;
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> ModBrowserScreen::hints(const UiEnv& /*env*/) const {
    std::vector<ButtonHint> hints{{ButtonHint::Button::Square, "Search"}, {ButtonHint::Button::Triangle, "Order"}};
    if (!query_.text.empty()) hints.push_back({ButtonHint::Button::Options, "Clear search"});
    hints.push_back({ButtonHint::Button::Circle, "Back"});
    hints.push_back({ButtonHint::Button::Cross, "Details"});
    return hints;
}

void ModBrowserScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    canvas.drawText(gameName_ + " - Mods", {content.x, content.y, 1100, 56},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    std::string toolbar = strings::concat("Order: ", orderLabel(query_.order));
    if (!query_.text.empty()) toolbar += "   Search: \"" + query_.text + "\"";
    canvas.drawText(toolbar, {content.x + 1100, content.y, content.w - 1100, 56},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Right, false});
    const std::string context =
        query_.game ? "Labels are for your installed version " + query_.game->version + " (" + query_.game->titleId + ")."
                    : "This game is not installed: labels show what the catalogue claims, not what was checked "
                      "for your console.";
    canvas.drawText(context, {content.x, content.y + 52, content.w, 40},
                    TextStyle{FontRole::Caption, query_.game ? theme::kTextSecondary : theme::kWarning, TextAlign::Left,
                              false});

    const Rect listArea{content.x, content.y + 104, content.w, content.h - 104};
    const ModListView& view = env.state.modList;
    const bool current = sameQuery(view.query, query_);
    if (!current || (!view.page && view.loading)) {
        loadingMessage(canvas, listArea, "Loading mods...", env.time);
        return;
    }
    if (view.error) {
        const bool missing = view.error->code == ErrorCode::NotFound;
        draw::messagePanel(canvas, {listArea.x, listArea.y, listArea.w, 300},
                           missing ? "No mods for this game yet" : "Could not load the mods", view.error->message,
                           missing ? theme::kNeutral : theme::kError);
        return;
    }
    if (!view.page) {
        return;
    }
    if (view.loading) {
        draw::spinner(canvas, content.x + 1060, content.y + 28, env.time);
    }
    const auto& mods = view.page->mods;
    if (mods.empty()) {
        draw::messagePanel(canvas, {listArea.x, listArea.y, listArea.w, 300}, "No matching mods",
                           query_.text.empty() ? "The catalogue lists no mods for this game."
                                               : "No mod matches \"" + query_.text +
                                                     "\". Press SQUARE to search again or OPTIONS to clear the search.",
                           theme::kNeutral);
        return;
    }
    list_.setCount(static_cast<int>(mods.size()));
    list_.setVisibleRows(kVisibleRows);
    constexpr int kRowHeight = 124;
    constexpr int kRowGap = 11;
    for (int i = list_.firstVisible(); i < std::min(list_.count(), list_.firstVisible() + kVisibleRows); ++i) {
        const auto& mod = mods[static_cast<std::size_t>(i)];
        const Rect box{listArea.x, listArea.y + (i - list_.firstVisible()) * (kRowHeight + kRowGap), listArea.w, kRowHeight};
        const bool focused = i == list_.focus();
        canvas.fillRoundedRect(box, 14, focused ? theme::kPanelRaised : theme::kPanel);
        if (focused) draw::focusRing(canvas, box, 14);
        const Rect thumb{box.x + 8, box.y + 8, kThumbWidth, kThumbHeight};
        const std::string key = env.commands.remoteImageKey(mod.thumbnailUrl, kThumbWidth, kThumbHeight);
        if (key.empty() || !canvas.drawImage(key, thumb)) draw::placeholderArt(canvas, thumb, mod.name);

        const std::string_view label = theme::badgeLabel(mod.compatibility);
        const int badgeW = badgeWidth(canvas, label);
        statusBadge(canvas, box.right() - badgeW - 24, box.y + 14, mod.compatibility);
        const int textX = thumb.right() + 28;
        const int textW = box.right() - textX - badgeW - 48;
        canvas.drawText(mod.name, {textX, box.y + 6, textW, 46},
                        TextStyle{FontRole::Body, theme::kTextPrimary, TextAlign::Left, true});
        std::string meta = "by " + (mod.author.empty() ? std::string("unknown author") : mod.author);
        if (!mod.version.empty()) meta += "   v" + mod.version;
        if (mod.downloadSize) meta += "   " + strings::formatBytes(*mod.downloadSize);
        if (!mod.updatedAt.empty()) meta += "   updated " + mod.updatedAt.substr(0, 10);
        canvas.drawText(meta, {textX, box.y + 50, box.right() - textX - 24, 36},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
        canvas.drawText(mod.shortDescription, {textX, box.y + 84, box.right() - textX - 24, 34},
                        TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Left, false});
    }
    std::string position = strings::concat(list_.focus() + 1, " of ", mods.size());
    if (view.page->total > mods.size()) position += strings::concat(" (first ", mods.size(), " of ", view.page->total, ")");
    canvas.drawText(position, {content.x, content.y + 52, content.w, 40},
                    TextStyle{FontRole::Small, theme::kTextSecondary, TextAlign::Right, false});
}

// ---------------------------------------------------------------- ModDetailScreen

namespace {

enum class ModAction { Install, Screenshots, Back };
constexpr std::array<ModAction, 3> kModActions{ModAction::Install, ModAction::Screenshots, ModAction::Back};

struct DocLine {
    std::string text;
    FontRole role = FontRole::Caption;
    Color color = theme::kTextSecondary;
    bool bold = false;
    std::optional<Color> dot;  // check mark colour
};

constexpr int kDocLineHeight = 40;

void addWrapped(ICanvas& canvas, std::vector<DocLine>& lines, std::string_view text, int width, FontRole role,
                Color color, bool bold = false, std::optional<Color> dot = std::nullopt) {
    bool first = true;
    for (auto& line : wrapText(canvas, text, width - (dot ? 40 : 0), role, bold)) {
        lines.push_back(DocLine{std::move(line), role, color, bold, first ? dot : std::nullopt});
        first = false;
    }
}

void addHeading(std::vector<DocLine>& lines, std::string text) {
    if (!lines.empty()) lines.push_back(DocLine{});
    lines.push_back(DocLine{std::move(text), FontRole::Body, theme::kTextPrimary, true, std::nullopt});
}

std::vector<DocLine> buildDocument(ICanvas& canvas, const providers::ModDetails& d, int width) {
    std::vector<DocLine> lines;
    addHeading(lines, "Compatibility with your console");
    for (const auto& check : d.checks) {
        addWrapped(canvas, lines, check.label + ": " + check.value, width, FontRole::Caption, theme::kTextPrimary, false,
                   markColor(check.mark));
    }
    for (const auto& reason : d.compatibilityReasons) {
        addWrapped(canvas, lines, reason, width, FontRole::Caption, theme::kWarning);
    }

    if (!d.description.empty()) {
        addHeading(lines, "Description");
        addWrapped(canvas, lines, d.description, width, FontRole::Caption, theme::kTextSecondary);
    }

    if (!d.files.empty()) {
        addHeading(lines, "Files");
        for (const auto& file : d.files) {
            std::string text = file.displayName.empty() ? file.fileId : file.displayName;
            text += " \xC2\xB7 " + strings::formatBytes(file.sizeBytes);
            if (!file.format.empty()) text += " \xC2\xB7 " + file.format;
            addWrapped(canvas, lines, text, width, FontRole::Caption, theme::kTextPrimary);
            if (!file.sha256.empty()) {
                addWrapped(canvas, lines, "SHA-256 " + file.sha256, width, FontRole::Small, theme::kTextSecondary);
            }
        }
    }

    addHeading(lines, "Details");
    auto row = [&](std::string_view label, const std::string& value) {
        if (!value.empty()) {
            addWrapped(canvas, lines, std::string(label) + ": " + value, width, FontRole::Caption, theme::kTextSecondary);
        }
    };
    auto join = [](const std::vector<std::string>& values) {
        std::string out;
        for (const auto& value : values) out += (out.empty() ? "" : ", ") + value;
        return out;
    };
    row("Game versions checked by the author", join(d.gameVersions));
    row("Title IDs", join(d.titleIds));
    row("Mod type", d.modType);
    row("Engine", d.engine);
    row("License", d.license);
    row("Homepage", d.homepage);
    if (!d.summary.categories.empty()) row("Categories", join(d.summary.categories));
    for (const auto& dependency : d.dependencies) {
        std::string kind;
        switch (dependency.kind) {
            case providers::ModDependency::Kind::Requires: kind = "Requires"; break;
            case providers::ModDependency::Kind::Recommends: kind = "Recommends"; break;
            case providers::ModDependency::Kind::ConflictsWith: kind = "Conflicts with"; break;
            case providers::ModDependency::Kind::LoadAfter: kind = "Loads after"; break;
            case providers::ModDependency::Kind::LoadBefore: kind = "Loads before"; break;
        }
        row(kind, dependency.target.modId);
    }
    return lines;
}

}  // namespace

ModDetailScreen::ModDetailScreen(providers::ModRef ref, std::string gameName, std::optional<providers::GameContext> game)
    : ref_(std::move(ref)), gameName_(std::move(gameName)), game_(std::move(game)) {}

const providers::ModDetails* ModDetailScreen::details(const UiEnv& env) const {
    const ModDetailView& view = env.state.modDetail;
    if (!(view.ref == ref_) || !view.details) return nullptr;
    return &*view.details;
}

void ModDetailScreen::update(UiEnv& env) {
    if (!requested_ || !(env.state.modDetail.ref == ref_)) {
        requested_ = true;
        env.commands.loadModDetails(ref_, game_);
    }
}

NavRequest ModDetailScreen::handle(Action action, UiEnv& env) {
    actions_.setCount(static_cast<int>(kModActions.size()));
    actions_.setVisibleRows(static_cast<int>(kModActions.size()));
    if (action == Action::Back) {
        return NavRequest::pop();
    }
    if (action == Action::PageDown || action == Action::PageUp) {
        scroll_ = std::clamp(scroll_ + (action == Action::PageDown ? 6 : -6), 0, maxScroll_);
        return NavRequest::none();
    }
    if (actions_.handle(action)) {
        return NavRequest::none();
    }
    if (action != Action::Confirm) {
        return NavRequest::none();
    }
    const providers::ModDetails* d = details(env);
    switch (kModActions[static_cast<std::size_t>(actions_.focus())]) {
        case ModAction::Install:
            if (d == nullptr) {
                env.showToast("The mod details are still loading.");
            } else if (!d->installable) {
                std::string reason = d->compatibilityReasons.empty() ? std::string("its compatibility label")
                                                                     : d->compatibilityReasons.front();
                env.showToast("Akeno will not install this mod: " + reason, ToastKind::Error);
            } else {
                env.showToast(std::string(kNotYetInstallable), ToastKind::Warning);
            }
            break;
        case ModAction::Screenshots:
            if (d != nullptr && !d->screenshots.empty()) {
                return NavRequest::push(std::make_unique<ScreenshotViewerScreen>(d->screenshots, 0));
            }
            env.showToast("This mod has no screenshots.");
            break;
        case ModAction::Back:
            return NavRequest::pop();
    }
    return NavRequest::none();
}

std::vector<ButtonHint> ModDetailScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::L2R2, "Scroll"}, {ButtonHint::Button::Circle, "Back"}, {ButtonHint::Button::Cross, "Select"}};
}

void ModDetailScreen::render(ICanvas& canvas, UiEnv& env) {
    const Rect content = theme::kContent;
    const ModDetailView& view = env.state.modDetail;
    const providers::ModDetails* d = details(env);
    if (d == nullptr) {
        if (view.ref == ref_ && view.error && !view.loading) {
            draw::messagePanel(canvas, {content.x, content.y, content.w, 300}, "Could not load this mod",
                               view.error->message + " Press CIRCLE to go back.", theme::kError);
        } else {
            loadingMessage(canvas, content, "Loading mod details...", env.time);
        }
        return;
    }

    // Left column: picture, labels, actions.
    const Rect hero{content.x, content.y, kHeroWidth, kHeroHeight};
    const std::string heroUrl = d->screenshots.empty() ? d->summary.thumbnailUrl : d->screenshots.front().url;
    const std::string heroKey = env.commands.remoteImageKey(heroUrl, kHeroWidth, kHeroHeight);
    if (heroKey.empty() || !canvas.drawImage(heroKey, hero)) draw::placeholderArt(canvas, hero, d->summary.name);
    int badgeX = content.x;
    badgeX += statusBadge(canvas, badgeX, hero.bottom() + 20, d->summary.compatibility) + 16;
    draw::badge(canvas, badgeX, hero.bottom() + 20, "RISK: " + (d->risk.empty() ? std::string("UNKNOWN") : d->risk),
                riskColor(d->risk));

    actions_.setCount(static_cast<int>(kModActions.size()));
    const std::array<std::string, 3> labels{
        "Download & install",
        d->screenshots.empty() ? std::string("No screenshots") : strings::concat("Screenshots (", d->screenshots.size(), ")"),
        "Back"};
    const int buttonY = hero.bottom() + 84;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const ModAction kind = kModActions[static_cast<std::size_t>(i)];
        const bool enabled = kind == ModAction::Back || (kind == ModAction::Screenshots && !d->screenshots.empty());
        draw::button(canvas, {content.x, buttonY + i * 80, kHeroWidth, 66}, labels[static_cast<std::size_t>(i)],
                     actions_.focus() == i, enabled);
    }
    canvas.drawText(d->installable ? "Downloading is not available in this version."
                                   : "This mod cannot be installed on PS5.",
                    {content.x, buttonY + 3 * 80, kHeroWidth, 36},
                    TextStyle{FontRole::Small, d->installable ? theme::kTextDisabled : theme::kError, TextAlign::Left,
                              false});

    // Right column: name, author and a scrollable document.
    const int infoX = content.x + kHeroWidth + 50;
    const int infoW = content.right() - infoX;
    canvas.drawText(d->summary.name, {infoX, content.y, infoW, 64},
                    TextStyle{FontRole::Title, theme::kTextPrimary, TextAlign::Left, true});
    std::string byline = "by " + (d->summary.author.empty() ? std::string("unknown author") : d->summary.author);
    if (!d->summary.version.empty()) byline += "   version " + d->summary.version;
    byline += "   for " + gameName_;
    canvas.drawText(byline, {infoX, content.y + 66, infoW, 40},
                    TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
    if (view.loading) draw::spinner(canvas, content.right() - 30, content.y + 30, env.time);

    const Rect doc{infoX, content.y + 124, infoW, content.h - 124};
    const auto lines = buildDocument(canvas, *d, doc.w - 30);
    const int visible = doc.h / kDocLineHeight;
    maxScroll_ = std::max(0, static_cast<int>(lines.size()) - visible);
    scroll_ = std::clamp(scroll_, 0, maxScroll_);
    for (int i = 0; i < visible && scroll_ + i < static_cast<int>(lines.size()); ++i) {
        const DocLine& line = lines[static_cast<std::size_t>(scroll_ + i)];
        const int y = doc.y + i * kDocLineHeight;
        int x = doc.x;
        if (line.dot) {
            canvas.fillCircle(x + 12, y + kDocLineHeight / 2, 10, *line.dot);
            x += 40;
        }
        canvas.drawText(line.text, {x, y, doc.right() - x - 30, kDocLineHeight},
                        TextStyle{line.role, line.color, TextAlign::Left, line.bold});
    }
    if (maxScroll_ > 0) {
        // Scroll bar.
        const Rect track{doc.right() - 10, doc.y, 8, visible * kDocLineHeight};
        canvas.fillRoundedRect(track, 4, theme::kPanelRaised);
        const int thumbH = std::max(40, track.h * visible / static_cast<int>(lines.size()));
        const int thumbY = track.y + (track.h - thumbH) * scroll_ / maxScroll_;
        canvas.fillRoundedRect({track.x, thumbY, track.w, thumbH}, 4, theme::kAccent);
    }
}

// ---------------------------------------------------------------- ScreenshotViewerScreen

NavRequest ScreenshotViewerScreen::handle(Action action, UiEnv& /*env*/) {
    const int count = static_cast<int>(screenshots_.size());
    switch (action) {
        case Action::Back:
            return NavRequest::pop();
        case Action::Left:
            if (count > 0) index_ = (index_ + count - 1) % count;
            break;
        case Action::Right:
        case Action::Confirm:
            if (count > 0) index_ = (index_ + 1) % count;
            break;
        default:
            break;
    }
    return NavRequest::none();
}

std::vector<ButtonHint> ScreenshotViewerScreen::hints(const UiEnv& /*env*/) const {
    return {{ButtonHint::Button::LeftRight, "Previous / next"}, {ButtonHint::Button::Circle, "Close"}};
}

void ScreenshotViewerScreen::render(ICanvas& canvas, UiEnv& env) {
    canvas.fillRect({0, 0, theme::kScreenWidth, theme::kScreenHeight}, Color{0, 0, 0, 255});
    if (screenshots_.empty()) {
        return;
    }
    index_ = std::clamp(index_, 0, static_cast<int>(screenshots_.size()) - 1);
    const auto& shot = screenshots_[static_cast<std::size_t>(index_)];
    const Rect area{160, 40, 1600, 900};
    const std::string key = env.commands.remoteImageKey(shot.url, area.w, area.h);
    if (key.empty() || !canvas.drawImage(key, area)) {
        canvas.drawText(key.empty() ? "This screenshot address is not allowed." : "Loading screenshot...",
                        {area.x, area.y + area.h / 2 - 25, area.w, 50},
                        TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Center, false});
    }
    std::string caption = strings::concat(index_ + 1, " / ", screenshots_.size());
    if (!shot.caption.empty()) caption = shot.caption + "   " + caption;
    canvas.drawText(caption, {area.x, area.bottom() + 4, area.w, 52},
                    TextStyle{FontRole::Caption, theme::kTextPrimary, TextAlign::Center, false});
}

}  // namespace akeno::ui
