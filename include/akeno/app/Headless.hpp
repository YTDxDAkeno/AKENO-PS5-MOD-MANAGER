// SPDX-License-Identifier: GPL-3.0-or-later
// Non-interactive modes. They are the first rungs of the hardware testing ladder
// (docs/safety-model.md §9): they only read system state and write reports into logs/.
#pragma once

#include "akeno/app/AppContext.hpp"

namespace akeno::app {

// Runs the system check, prints the report, writes logs/system-check-<ts>.txt and shows a
// short notification. Returns 0 when the check ran (whatever its findings), 1 otherwise.
int runSelfCheck(AppContext& context);

// Asks ShadowMountPlus for the game list, prints it and writes logs/games-<ts>.txt.
// Returns 0 on success, 1 if the list could not be read.
int runListGames(AppContext& context);

}  // namespace akeno::app
