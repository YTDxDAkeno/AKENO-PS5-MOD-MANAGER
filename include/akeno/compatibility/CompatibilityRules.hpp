// SPDX-License-Identifier: GPL-3.0-or-later
// Turns a catalogue claim plus the installed game into the label shown to the user
// (docs/compatibility.md). The catalogue's word is never taken at face value: VERIFIED
// requires the installed game version to be one the mod was verified against.
// Archive contents (native code, fakelib, paths) are evaluated in Phase 4 and can only
// lower the result.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "akeno/mods/Catalog.hpp"
#include "akeno/providers/IModProvider.hpp"

namespace akeno::compatibility {

enum class CheckMark { Pass, Warn, Fail, Unknown };

struct CompatibilityLine {
    std::string label;   // "PS5 Title ID"
    std::string value;   // "PPSA01234"
    CheckMark mark = CheckMark::Unknown;
};

enum class Risk { Low, Medium, High, Unknown };

struct CompatibilityResult {
    providers::CompatibilityStatus status = providers::CompatibilityStatus::Unknown;
    Risk risk = Risk::Unknown;
    std::vector<CompatibilityLine> lines;
    std::vector<std::string> reasons;  // why the status is not better
    bool installable = false;          // allowed by the rules (the build may still lack installation)
    bool needsConfirmation = false;    // experimental installs need a deliberate confirmation
};

using InstalledGame = providers::GameContext;

// Phase 2 rules: manifest metadata versus the installed game.
CompatibilityResult evaluateManifest(const mods::ModManifest& manifest, const std::optional<InstalledGame>& game);

// Label for list rows, from the catalogue summary. Follows the same version rule as
// evaluateManifest(): VERIFIED only when the installed version is listed.
providers::CompatibilityStatus summaryLabel(const mods::ModSummaryEntry& entry, const std::optional<InstalledGame>& game);

std::string_view toString(Risk risk) noexcept;

}  // namespace akeno::compatibility
