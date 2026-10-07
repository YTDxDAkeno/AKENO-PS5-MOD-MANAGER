// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/OperationJournal.hpp"

#include <system_error>

#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/security/PathGuard.hpp"

namespace akeno {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxJournalBytes = 256 * 1024;

json::Json toJson(const OperationState& state) {
    json::Json staging = json::Json::array();
    for (const auto& path : state.stagingPaths) {
        staging.push_back(path.string());
    }
    return json::Json{
        {"schemaVersion", OperationState::kSchemaVersion},
        {"operationId", state.operationId},
        {"kind", state.kind},
        {"titleId", state.titleId},
        {"description", state.description},
        {"step", state.step},
        {"completedSteps", state.completedSteps},
        {"stagingPaths", staging},
        {"activeOverlayTouched", state.activeOverlayTouched},
        {"startedAt", state.startedAt},
        {"updatedAt", state.updatedAt},
    };
}

Result<OperationState> fromJson(const json::Json& document) {
    auto version = json::getInt(document, "schemaVersion");
    if (!version || *version != OperationState::kSchemaVersion) {
        return makeError(ErrorCode::SchemaError, "The recovery journal has an unknown format.");
    }
    OperationState state;
    state.operationId = json::getString(document, "operationId").value_or("");
    state.kind = json::getString(document, "kind").value_or("");
    state.titleId = json::getString(document, "titleId").value_or("");
    state.description = json::displayString(document, "description");
    state.step = json::getString(document, "step").value_or("");
    state.activeOverlayTouched = json::getBool(document, "activeOverlayTouched").value_or(true);
    state.startedAt = json::getString(document, "startedAt").value_or("");
    state.updatedAt = json::getString(document, "updatedAt").value_or("");
    if (const auto* steps = json::getArray(document, "completedSteps")) {
        for (const auto& step : *steps) {
            if (step.is_string()) state.completedSteps.push_back(step.get<std::string>());
        }
    }
    if (const auto* paths = json::getArray(document, "stagingPaths")) {
        for (const auto& path : *paths) {
            if (path.is_string()) state.stagingPaths.emplace_back(path.get<std::string>());
        }
    }
    if (state.operationId.empty() || state.kind.empty()) {
        return makeError(ErrorCode::SchemaError, "The recovery journal is incomplete.");
    }
    return state;
}

std::string kindNoun(const std::string& kind) {
    if (kind == "install") return "installation";
    if (kind == "activate") return "overlay activation";
    if (kind == "remove") return "mod removal";
    if (kind == "update") return "mod update";
    if (kind == "rollback") return "rollback";
    if (kind == "download") return "download";
    if (kind == "check") return "mod check";
    return "operation";
}

}  // namespace

RecoveryAdvice adviseRecovery(const OperationState& state) {
    RecoveryAdvice advice;
    advice.headline = "Interrupted " + kindNoun(state.kind) + " detected.";
    advice.canCleanStaging = !state.stagingPaths.empty();
    advice.needsOverlayRestore = state.activeOverlayTouched;
    if (state.activeOverlayTouched) {
        advice.explanation =
            "The game's mod overlay was being switched. Cleaning up deletes Akeno's temporary files. "
            "If the overlay is missing afterwards, the game simply runs unmodified (Vanilla); install "
            "the mod again from Downloads to turn it back on.";
    } else {
        advice.explanation = "No active overlay was changed.";
    }
    return advice;
}

Result<std::optional<OperationState>> OperationJournal::load() const {
    std::error_code ec;
    if (!fs::exists(file_, ec)) {
        return std::optional<OperationState>{};
    }
    auto contents = security::readFileBounded(file_, kMaxJournalBytes);
    if (!contents) {
        return std::move(contents).error();
    }
    auto document = json::parseBounded(contents.value(), kMaxJournalBytes);
    if (!document) {
        return makeError(ErrorCode::ParseError, "The recovery journal is damaged.",
                         document.error().describe());
    }
    auto state = fromJson(document.value());
    if (!state) {
        return std::move(state).error();
    }
    return std::optional<OperationState>(std::move(state).value());
}

Status OperationJournal::persist() {
    if (!current_) {
        return makeError(ErrorCode::Internal, "No operation in progress.");
    }
    current_->updatedAt = strings::utcTimestamp();
    return fs_.writeFileAtomic(file_, toJson(*current_).dump(2));
}

Status OperationJournal::begin(OperationState state) {
    if (current_) {
        return makeError(ErrorCode::Busy, "Another operation is already in progress.", current_->operationId);
    }
    state.startedAt = strings::utcTimestamp();
    current_ = std::move(state);
    auto status = persist();
    if (!status) {
        current_.reset();
    }
    return status;
}

Status OperationJournal::recordStep(const std::string& step) {
    if (!current_) return makeError(ErrorCode::Internal, "No operation in progress.");
    current_->step = step;
    return persist();
}

Status OperationJournal::markStepCompleted(const std::string& step) {
    if (!current_) return makeError(ErrorCode::Internal, "No operation in progress.");
    current_->completedSteps.push_back(step);
    return persist();
}

Status OperationJournal::addStagingPath(const fs::path& path) {
    if (!current_) return makeError(ErrorCode::Internal, "No operation in progress.");
    current_->stagingPaths.push_back(path);
    return persist();
}

Status OperationJournal::markOverlayTouched() {
    if (!current_) return makeError(ErrorCode::Internal, "No operation in progress.");
    current_->activeOverlayTouched = true;
    return persist();
}

Status OperationJournal::complete() {
    auto status = fs_.removeFile(file_);
    if (status) {
        current_.reset();
    }
    return status;
}

Status OperationJournal::cleanStaging(const OperationState& state, const fs::path& stagingRoot) {
    auto root = security::normalizeAbsolute(stagingRoot.native());
    if (!root) {
        return std::move(root).error();
    }
    for (const fs::path& path : state.stagingPaths) {
        auto normalized = security::normalizeAbsolute(path.native());
        if (!normalized || !security::isWithin(root.value(), normalized.value()) ||
            normalized.value() == root.value()) {
            return makeError(ErrorCode::SafetyViolation,
                             "The recovery journal names a path outside the staging area. Nothing was deleted.",
                             path.string());
        }
    }
    for (const fs::path& path : state.stagingPaths) {
        AKENO_TRY(fs_.removeTree(path));
    }
    return fs_.removeFile(file_);
}

std::vector<std::string> removeLeftoverStaging(const security::SafeFs& fs, const fs::path& stagingRoot) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(stagingRoot, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (strings::startsWith(name, "check-") || strings::startsWith(name, "link-probe-")) names.push_back(name);
    }
    std::vector<std::string> removed;
    for (const std::string& name : names) {
        if (fs.removeTree(stagingRoot / name)) removed.push_back(name);
    }
    return removed;
}

}  // namespace akeno
