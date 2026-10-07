// SPDX-License-Identifier: GPL-3.0-or-later
// Crash-recovery journal (operation_state.json). An operation that changes state writes its
// intent here before starting and records each step, so the next launch can tell exactly
// what was interrupted and what is safe to clean up.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/security/SafeFs.hpp"

namespace akeno {

struct OperationState {
    static constexpr int kSchemaVersion = 1;

    std::string operationId;   // unique id, e.g. "install-PPSA00000-20261007-112233"
    std::string kind;          // "install", "activate", "remove", "update", "rollback"
    std::string titleId;       // empty when not title-specific
    std::string description;   // human readable: "Installing Example Outfit 1.0.0"
    std::string step;          // last step that started
    std::vector<std::string> completedSteps;
    std::vector<std::filesystem::path> stagingPaths;  // created by this operation; safe to delete
    bool activeOverlayTouched = false;                 // true once the live overlay may differ
    std::string startedAt;
    std::string updatedAt;
};

// What the user is told after an interrupted operation.
struct RecoveryAdvice {
    std::string headline;     // "Interrupted installation detected."
    std::string explanation;  // "No active overlay was changed."
    bool canCleanStaging = false;
    bool needsOverlayRestore = false;
};

RecoveryAdvice adviseRecovery(const OperationState& state);

class OperationJournal {
public:
    OperationJournal(std::filesystem::path file, const security::SafeFs& fs) : file_(std::move(file)), fs_(fs) {}

    // nullopt when no operation was in progress.
    Result<std::optional<OperationState>> load() const;

    Status begin(OperationState state);
    Status recordStep(const std::string& step);
    Status markStepCompleted(const std::string& step);
    Status addStagingPath(const std::filesystem::path& path);
    Status markOverlayTouched();
    Status complete();

    // Removes staging paths of an interrupted operation, then clears the journal. Paths outside
    // `stagingRoot` are refused (the journal itself is treated as untrusted input).
    Status cleanStaging(const OperationState& state, const std::filesystem::path& stagingRoot);

    const std::optional<OperationState>& current() const noexcept { return current_; }

private:
    Status persist();

    std::filesystem::path file_;
    const security::SafeFs& fs_;
    std::optional<OperationState> current_;
};

// Removes what Akeno itself leaves in staging ("check-*" folders, "link-probe-*" files) when no
// operation is recorded: leftovers of builds that could not delete them. Call it only when the
// journal is empty. Other names are left alone. Returns the names removed.
std::vector<std::string> removeLeftoverStaging(const security::SafeFs& fs, const std::filesystem::path& stagingRoot);

}  // namespace akeno
