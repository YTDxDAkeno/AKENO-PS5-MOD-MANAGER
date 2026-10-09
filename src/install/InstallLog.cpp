// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/install/InstallLog.hpp"

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"

namespace akeno::install {

using logging::logger;

std::string_view toString(Stage stage) noexcept {
    switch (stage) {
        case Stage::DownloadComplete: return "download-complete";
        case Stage::ArchiveChecked: return "archive-checked";
        case Stage::FilesInstalled: return "files-installed";
        case Stage::OverlayPublished: return "overlay-published";
        case Stage::OverlayMounted: return "overlay-mounted";
        case Stage::GameConsumedFiles: return "game-consumed-files";
        case Stage::BehaviourVerified: return "behaviour-verified";
    }
    return "?";
}

std::string_view toString(StageResult result) noexcept {
    switch (result) {
        case StageResult::Ok: return "ok";
        case StageResult::Failed: return "failed";
        case StageResult::Refused: return "refused";
        case StageResult::RolledBack: return "rolled-back";
        case StageResult::NotObserved: return "not-observed";
    }
    return "?";
}

void logStage(Stage stage, StageResult result, const std::string& context, const std::string& detail) {
    std::string line = strings::concat("stage ", static_cast<int>(stage), "/7 ", toString(stage), ": ", toString(result),
                                       " | ", context);
    if (!detail.empty()) line += " | " + detail;
    if (result == StageResult::Failed || result == StageResult::Refused || result == StageResult::RolledBack) {
        logger().warn("install", line);
    } else {
        logger().info("install", line);
    }
    logger().flush();
}

void logUnobservedStages(const std::string& context) {
    logStage(Stage::OverlayMounted, StageResult::NotObserved, context,
             "Akeno cannot see ShadowMountPlus mounts; check SMP's debug.log after starting the game");
    logStage(Stage::GameConsumedFiles, StageResult::NotObserved, context,
             "no evidence that the game opened the mod's files");
    logStage(Stage::BehaviourVerified, StageResult::NotObserved, context,
             "the mod's effect must be confirmed in the game by a person");
}

}  // namespace akeno::install
