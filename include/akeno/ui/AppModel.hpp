// SPDX-License-Identifier: GPL-3.0-or-later
// The boundary between screens and the application. Screens read AppViewState (updated on the
// UI thread only) and ask for work through IAppCommands; they never touch services directly.
// Tests drive screens with a hand-built state and a fake command implementation.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "akeno/app/SystemCheck.hpp"
#include "akeno/core/OperationJournal.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/database/SettingsStore.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/logging/Logger.hpp"

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

struct AppViewState {
    SystemCheckView systemCheck;
    LibraryView library;
    database::Settings settings;
    bool settingsPersistent = true;            // false when the database is unavailable
    std::optional<RecoveryView> recovery;
    AboutInfo about;
};

enum class ToastKind { Info, Success, Warning, Error };

class IAppCommands {
public:
    virtual ~IAppCommands() = default;

    virtual void runSystemCheck() = 0;
    virtual void refreshLibrary() = 0;
    virtual Status saveSettings(const database::Settings& settings) = 0;
    virtual Result<std::string> exportDiagnostics() = 0;
    virtual std::vector<logging::LogRecord> recentLogs() = 0;
    // Deletes the staging data of an interrupted operation and clears the journal.
    virtual Status cleanInterruptedOperation() = 0;
    // Hides the recovery prompt for this session; the journal is kept for next time.
    virtual void postponeRecovery() = 0;
    // Returns the image key for a game's icon at `size` pixels and starts loading it if needed.
    virtual std::string gameIconKey(const games::GameInfo& game, int size) = 0;
    virtual void requestQuit() = 0;
};

}  // namespace akeno::ui
