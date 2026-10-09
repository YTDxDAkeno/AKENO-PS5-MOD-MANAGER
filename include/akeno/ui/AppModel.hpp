// SPDX-License-Identifier: GPL-3.0-or-later
// The boundary between screens and the application. Screens read AppViewState (updated on the
// UI thread only) and ask for work through IAppCommands; they never touch services directly.
// Tests drive screens with a hand-built state and a fake command implementation.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "akeno/app/SystemCheck.hpp"
#include "akeno/core/OperationJournal.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/database/SettingsStore.hpp"
#include "akeno/downloads/DownloadTypes.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/providers/IModProvider.hpp"

namespace akeno::ui {

struct SystemCheckView {
    bool running = false;
    std::vector<app::CheckResult> results;     // grows while the check runs
    std::optional<app::SystemReport> report;   // set when finished
};

struct LibraryView {
    bool loading = false;
    bool everLoaded = false;
    std::optional<Error> error;
    std::vector<games::GameInfo> games;        // already filtered and sorted per settings
    std::size_t totalGames = 0;                // before filtering
    std::string refreshedAt;
    std::vector<std::string> versionChanges;
};

struct RecoveryView {
    OperationState state;
    RecoveryAdvice advice;
};

struct AboutInfo {
    std::string version;
    std::string revision;
    std::string target;
    std::string compiler;
    std::string testingStatus;
    std::string platformName;
    std::string firmware;
    std::string dataRoot;
    std::string networkStack;
    std::string databaseEngine;
};

struct CatalogView {
    bool configured = false;                    // a valid catalogue address is set
    std::string source;                         // catalogue address shown to the user
    bool overridden = false;                    // set for this session with --catalogue-url
    std::optional<Error> configurationError;
    bool loading = false;
    bool loaded = false;
    std::optional<Error> error;
    std::vector<providers::ProviderGame> games;

    const providers::ProviderGame* findByTitleId(std::string_view titleId) const {
        for (const auto& game : games) {
            for (const auto& id : game.titleIds) {
                if (id == titleId) return &game;
            }
        }
        return nullptr;
    }
};

// The mod list currently shown (one list at a time).
struct ModListView {
    std::uint64_t request = 0;                  // increases with every load; stale results are dropped
    providers::SearchQuery query;
    bool loading = false;
    std::optional<Error> error;
    std::optional<providers::ModPage> page;
};

struct ModDetailView {
    std::uint64_t request = 0;
    providers::ModRef ref;
    bool loading = false;
    std::optional<Error> error;
    std::optional<providers::ModDetails> details;
};

// Text entry (search). On the console the system keyboard is shown; on desktop builds the
// typed text is displayed by Akeno.
struct TextEntryView {
    bool active = false;
    bool systemKeyboard = false;                // the console's own keyboard dialog is used
    std::string prompt;
    std::string text;
};

struct DownloadsView {
    bool started = false;
    std::vector<downloads::DownloadInfo> items;  // oldest first
    std::optional<std::uint64_t> freeBytes;      // in the downloads folder
    std::uint64_t reserveBytes = 0;              // always kept free

    const downloads::DownloadInfo* find(const std::string& id) const {
        for (const auto& item : items) {
            if (item.record.id == id) return &item;
        }
        return nullptr;
    }
    // The newest download of this mod version.
    const downloads::DownloadInfo* findForMod(const providers::ModRef& mod, const std::string& version) const {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->record.request.mod == mod && it->record.request.modVersion == version) return &*it;
        }
        return nullptr;
    }
};

// The check (Phase 4) of one downloaded mod.
struct ModCheckView {
    std::string downloadId;
    bool running = false;
    mods::CheckPhase phase = mods::CheckPhase::Inspecting;
    double progress = 0.0;                       // of the current phase
    std::optional<Error> error;
    std::optional<mods::ModCheckReport> report;  // with conflicts and plan
};

// The mods Akeno keeps for one game (Phase 5).
struct InstalledModsSummary {
    std::size_t stored = 0;
    std::size_t enabled = 0;
    bool overlayActive = false;
    std::optional<Error> error;  // the list could not be read
};

// One stored mod in the Installed Mods tab.
struct InstalledModRow {
    std::string titleId;
    std::string gameName;
    std::string downloadId;
    std::string name;
    std::string version;
    std::string source;  // provider id
    bool enabled = false;
    bool overlayActive = false;  // of the game
    std::uint64_t bytes = 0;
};

enum class ToastKind { Info, Success, Warning, Error };

// Messages from background work, shown as toasts by the screen host.
struct Notice {
    std::uint64_t serial = 0;
    std::string text;
    ToastKind kind = ToastKind::Info;
};

struct DiagnosticView {
    bool running = false;
    std::string progress;
    std::string lastExport;
};

struct AppViewState {
    DiagnosticView diagnostics;
    SystemCheckView systemCheck;
    LibraryView library;
    CatalogView catalog;
    ModListView modList;
    ModDetailView modDetail;
    TextEntryView textEntry;
    DownloadsView downloads;
    ModCheckView check;
    std::vector<Notice> notices;               // the most recent ones; serials increase
    database::Settings settings;
    bool settingsPersistent = true;            // false when the database is unavailable
    std::optional<RecoveryView> recovery;
    AboutInfo about;
};

class IAppCommands {
public:
    virtual ~IAppCommands() = default;

    virtual void runSystemCheck() = 0;
    virtual void refreshLibrary() = 0;
    virtual Status saveSettings(const database::Settings& settings) = 0;
    // Quick by default; `deep` also hashes large game files (much slower).
    virtual void exportDiagnostics(const std::string& titleId = {}, bool deep = false) = 0;
    virtual void cancelDiagnostics() = 0;
    virtual std::vector<logging::LogRecord> recentLogs() = 0;
    // Deletes the staging data of an interrupted operation and clears the journal.
    virtual Status cleanInterruptedOperation() = 0;
    // Hides the recovery prompt for this session; the journal is kept for next time.
    virtual void postponeRecovery() = 0;
    // Returns the image key for a game's icon at `size` pixels and starts loading it if needed.
    virtual std::string gameIconKey(const games::GameInfo& game, int size) = 0;
    // Same for an image on the web (https only), scaled to fit width x height.
    virtual std::string remoteImageKey(const std::string& url, int width, int height) = 0;
    virtual void requestQuit() = 0;

    // Mod catalogue (Phase 2).
    virtual void loadCatalogGames(bool forceRefresh) = 0;
    virtual void loadModList(const providers::SearchQuery& query) = 0;
    virtual void loadModDetails(const providers::ModRef& ref, const std::optional<providers::GameContext>& game) = 0;

    // Downloads (Phase 3). startDownload resolves the mod's file through its provider and queues
    // it; the result is reported as a notice. Mods that the compatibility rules do not allow are
    // refused; EXPERIMENTAL ones need `confirmed`.
    virtual void startDownload(const providers::ModRef& ref, const std::optional<providers::GameContext>& game,
                               bool confirmed) = 0;
    virtual Status pauseDownload(const std::string& id) = 0;
    virtual Status resumeDownload(const std::string& id) = 0;
    virtual Status removeDownload(const std::string& id) = 0;

    // Checks a completed download (Phase 4): unpack into staging, analyse, plan, clean up.
    // A stored result is shown unless `again` is set.
    virtual void checkDownload(const std::string& id, bool again) = 0;
    virtual void cancelCheck() = 0;

    // Installing (Phase 5). installChecked keeps a checked download for its game and applies the
    // game's overlay; setGameVanilla turns every mod of a game off. Results arrive as notices.
    virtual void installChecked(const std::string& downloadId) = 0;
    virtual void setGameVanilla(const std::string& titleId) = 0;
    virtual InstalledModsSummary installedMods(const std::string& titleId) = 0;
    virtual bool installBusy() const = 0;
    // Installed Mods tab: every stored mod; turning one on or off rebuilds the game's overlay;
    // removing turns it off, rebuilds the overlay and deletes the stored copy.
    virtual std::vector<InstalledModRow> listInstalledMods() = 0;
    virtual void setInstalledModEnabled(const std::string& titleId, const std::string& downloadId, bool enabled) = 0;
    virtual void removeInstalledMod(const std::string& titleId, const std::string& downloadId) = 0;

    // Asks the user for text; `done` receives the text, or nullopt when cancelled.
    virtual void requestTextInput(const std::string& prompt, const std::string& initial,
                                  std::function<void(std::optional<std::string>)> done) = 0;
};

}  // namespace akeno::ui
