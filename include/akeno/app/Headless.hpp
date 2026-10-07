// SPDX-License-Identifier: GPL-3.0-or-later
// Non-interactive modes. They are the first rungs of the hardware testing ladder
// (docs/safety-model.md §9): they only read system state and write reports into logs/.
#pragma once

#include <cstdint>
#include <string>

#include "akeno/app/AppContext.hpp"

namespace akeno::app {

// Runs the system check, prints the report, writes logs/system-check-<ts>.txt and shows a
// short notification. Returns 0 when the check ran (whatever its findings), 1 otherwise.
// `reportOut`, if given, receives the report.
int runSelfCheck(AppContext& context, SystemReport* reportOut = nullptr);

// Asks ShadowMountPlus for the game list, prints it and writes logs/games-<ts>.txt.
// Returns 0 on success, 1 if the list could not be read.
int runListGames(AppContext& context);

// Ladder steps 5 to 9: downloads a small harmless archive through the real download engine into
// downloads/, checks its SHA-256, unpacks it into staging and analyses it, deletes the staging
// folder, describes the (not executed) install plan, deletes the download and writes
// logs/download-test-<ts>.txt. Returns 0 when every step passed, 1 otherwise.
struct DownloadTestSpec {
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
};
// The file in this repository (assets/test/download-test.zip), as published on the main branch.
DownloadTestSpec builtinDownloadTest();
int runDownloadTest(AppContext& context, const DownloadTestSpec& spec);

}  // namespace akeno::app
