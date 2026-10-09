// SPDX-License-Identifier: GPL-3.0-or-later
// Test installs of PC mods, and what the user reports after playing.
//
// A test install puts a PC mod's data-only Unreal package files into the game's package folder
// after the user confirmed the risks (compatibility::Assessment::testInstallAvailable says when
// that is offered). Nothing on the console can observe whether the game then loaded them, so the
// user reports what happened. The reports are kept with the mod (mods/<TITLE_ID>/state.json) and
// become evidence for later checks of the same game version, labelled as the user's own reports:
//   works      the folder is one this game version loads package files from, and this mod
//              version works on it
//   no-effect  nothing changed in the game: the game does not load it from there, or the change
//              is not visible; another placement can be tested
//   crashed    the game crashed or did not start: the mod is turned off, and the same mod version
//              is not offered for another test on this game version
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/compatibility/CompatibilityEngine.hpp"

namespace akeno::install {

// Where a test puts the package files: the PC convention <project>/content/paks/~mods, or the
// package folder itself, where the game's own containers are (no subfolder search needed).
enum class TestPlacement { ModsFolder, PaksFolder };
std::string_view toString(TestPlacement placement) noexcept;  // "mods-folder", "paks-folder"
std::optional<TestPlacement> parseTestPlacement(std::string_view text) noexcept;

enum class TestResult { Untested, Works, NoEffect, Crashed };
std::string_view toString(TestResult result) noexcept;  // "untested", "works", "no-effect", "crashed"
std::optional<TestResult> parseTestResult(std::string_view text) noexcept;
std::string_view describe(TestResult result) noexcept;  // for the user

// One test install and its reported result.
struct TestRecord {
    std::string titleId;
    std::string provider;
    std::string modId;
    std::string modVersion;
    std::string name;
    std::string gameVersion;  // of the game when the mod was installed
    std::string directory;    // game-relative folder the package files were put in
    TestPlacement placement = TestPlacement::ModsFolder;
    TestResult result = TestResult::Untested;
    std::string reportedAt;
};

// The user's reports for one title as a game adapter (EvidenceSource::Report). Untested and
// no-effect records add nothing; a game version that is unknown matches nothing.
std::shared_ptr<const compatibility::IGameAdapter> makeTestReportAdapter(std::string titleId,
                                                                         std::vector<TestRecord> records);

// The built-in registry plus the reports' adapter.
compatibility::Registry registryWithReports(const std::string& titleId, const std::vector<TestRecord>& records);

// Changes whenever a report that matters for checks changes (stored check results are redone).
// Empty when there is none; otherwise a SHA-256.
std::string evidenceStamp(const std::vector<TestRecord>& records);

}  // namespace akeno::install
