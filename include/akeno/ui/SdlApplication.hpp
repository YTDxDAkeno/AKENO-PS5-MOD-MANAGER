// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "akeno/app/AppContext.hpp"
#include "akeno/app/CommandLine.hpp"

namespace akeno::ui {

// Runs the controller user interface until the user exits. Returns a process exit code.
int runSdlApplication(app::AppContext& context, const app::CommandLine& commandLine);

}  // namespace akeno::ui
