// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace akeno::build {

std::string_view version();       // "0.1.0-alpha"
std::string_view gitRevision();   // full commit hash or "unknown"
std::string_view sourceFingerprint(); // SHA-256 of sources/configuration; identifies dirty builds too
std::string_view target();        // "ps5" or "host"
std::string_view compiler();      // compiler identification

// Honest testing status of this build, shown on the About screen and in reports.
std::string_view testingStatus();

}  // namespace akeno::build
