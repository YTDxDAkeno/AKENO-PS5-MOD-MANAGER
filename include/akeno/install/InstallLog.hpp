// SPDX-License-Identifier: GPL-3.0-or-later
// The installation ladder, written to the persistent Akeno log as one line per stage:
//
//   1 download-complete    the archive was downloaded and its SHA-256 verified
//   2 archive-checked      unpacked safely, analysed, mapped and assessed
//   3 files-installed      the mod's files are kept in Akeno's store and verified
//   4 overlay-published    the overlay folder was moved into ShadowMountPlus's backports
//   5 overlay-mounted      ShadowMountPlus mounted it          (not observable by Akeno)
//   6 game-consumed-files  the game opened the mod's files     (not observable by Akeno)
//   7 behaviour-verified   the mod's effect was confirmed      (needs a person on the console)
//
// Stages 5 to 7 are only ever logged as "not-observed": a published overlay is not evidence
// that it was mounted, and a mount is not evidence that the game used or accepted the files.
// Lines carry ids and hashes, never download URLs or keys (the logger redacts secrets too).
#pragma once

#include <string>
#include <string_view>

namespace akeno::install {

enum class Stage {
    DownloadComplete = 1,
    ArchiveChecked,
    FilesInstalled,
    OverlayPublished,
    OverlayMounted,
    GameConsumedFiles,
    BehaviourVerified,
};
std::string_view toString(Stage stage) noexcept;

enum class StageResult { Ok, Failed, Refused, RolledBack, NotObserved };
std::string_view toString(StageResult result) noexcept;

// "stage 3/7 files-installed: ok | title=PPSA28000 ... | detail". Written durably (flushed).
void logStage(Stage stage, StageResult result, const std::string& context, const std::string& detail = {});

// Logs stages 5 to 7 as not observed, after an overlay was published.
void logUnobservedStages(const std::string& context);

}  // namespace akeno::install
