// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4: what is inside a downloaded mod, what would be installed where, what is refused,
// which other mods it would collide with, and the install plan Phase 5 would follow. Nothing
// here writes to the game or executes anything; the plan is a description.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/core/AppPaths.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/mods/Catalog.hpp"
#include "akeno/providers/IModProvider.hpp"

namespace akeno::compatibility {
struct Assessment;
}

namespace akeno::mods {

struct ArchiveLayout;

enum class FileKind {
    Asset,          // game data (paks, textures, models, sounds, ...)
    Text,           // documentation, plain text
    Image,
    Config,         // ini, json, xml, cfg, ...
    Script,         // lua, python, shell, batch: never run by Akeno
    Archive,        // an archive inside the archive: not unpacked
    WindowsCode,    // PE executables and libraries (.exe, .dll, .asi)
    NativeCode,     // ELF/SELF/PRX: PS5 program code
    Junk,           // __MACOSX, .DS_Store, Thumbs.db
};
std::string_view toString(FileKind kind) noexcept;

// Uses the first bytes (magic numbers) before the name.
FileKind classifyFile(std::string_view path, std::string_view head);

enum class FindingLevel { Info, Warning, Blocker };
std::string_view toString(FindingLevel level) noexcept;

struct AnalysisFinding {
    FindingLevel level = FindingLevel::Info;
    std::string message;
    std::string path;  // an example file, may be empty
};

// What the install path is in the installed game (from its physical listing).
enum class TargetState {
    Unknown,       // the game's files could not be listed completely
    New,           // adds a file the game does not have
    Replaces,      // shadows a game file at the same path
    TypeConflict,  // a file where the game has a folder, or below a game file
};
std::string_view toString(TargetState state) noexcept;
std::optional<TargetState> parseTargetState(std::string_view text) noexcept;

struct AnalyzedFile {
    std::string archivePath;  // inside the archive
    std::string installPath;  // relative to the game's root; empty when not installed
    std::uint64_t size = 0;
    std::string sha256;
    FileKind kind = FileKind::Asset;
    TargetState target = TargetState::Unknown;
};

struct AnalysisInput {
    std::vector<archives::ExtractedFile> files;
    std::string archiveRoot;    // manifest installation.archiveRoot (validated relative path)
    std::string targetPrefix;   // manifest installation.targetPrefix
    // When set (from the archive layout analysis), it decides each file's install path instead
    // of archiveRoot/targetPrefix; files it does not list are not installed.
    std::optional<std::vector<std::pair<std::string, std::string>>> mapping;
    std::string titleId;
    std::optional<games::SourceType> sourceType;  // of the installed game, when known
    providers::CompatibilityStatus catalogueStatus = providers::CompatibilityStatus::Unknown;
    bool catalogueInstallable = false;  // from the compatibility rules
};

// What the content checks found, for the compatibility engine (counts of files).
struct ContentFlags {
    std::size_t nativeCode = 0;        // ELF/SELF/PRX: PS5 program code
    std::size_t windowsCode = 0;       // PE executables and libraries
    std::size_t scripts = 0;
    std::size_t nestedArchives = 0;
    bool fakelib = false;              // a fakelib folder (system libraries for SMP)
    bool systemFolders = false;        // sce_sys / sce_module
    bool executableReplacement = false;  // eboot.bin or another program at the game root
    bool pathTooLong = false;
    std::vector<std::string> loaders;  // PC-only loaders the files are made for
};

struct ModAnalysis {
    std::vector<AnalyzedFile> files;          // every file in the archive
    std::vector<AnalysisFinding> findings;    // blockers first
    std::size_t installCount = 0;
    std::uint64_t installBytes = 0;
    std::string engineHint;                   // "Unreal Engine packages", ...
    ContentFlags flags;
    providers::CompatibilityStatus status = providers::CompatibilityStatus::Unknown;
    bool installable = false;                 // no blockers and the label allows it

    bool hasBlockers() const;
};

ModAnalysis analyzeMod(const AnalysisInput& input);

// Files two mods would both place at the same path (compared without case).
struct Conflict {
    std::string otherId;
    std::string otherName;
    std::vector<std::string> paths;  // up to 20 examples
    std::size_t count = 0;
};
struct OtherMod {
    std::string id;
    std::string name;
    const ModAnalysis* analysis = nullptr;
};
std::vector<Conflict> predictConflicts(const ModAnalysis& mod, const std::vector<OtherMod>& others);

// The steps an install takes (src/install/OverlayManager.cpp), described before it starts.
struct PlanStep {
    std::string title;
    std::string detail;
};
struct InstallPlan {
    std::string titleId;
    std::filesystem::path modStore;      // mods/<TITLE_ID>/<download id>/
    std::filesystem::path overlayNext;   // overlays/<TITLE_ID>/overlay.next/
    std::string backportDirectory;       // default ShadowMountPlus location for the title
    std::vector<PlanStep> steps;
    std::vector<std::pair<std::string, std::string>> mapping;  // archive path -> backport path (first 200)
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    std::uint64_t overlayExtraBytes = 0; // space for overlay copies when hard links do not work
    std::uint64_t reserveBytes = 0;      // always kept free
    std::uint64_t requiredBytes = 0;     // stored copy + overlay copies + reserve
    bool changesGameFiles = false;       // always false: overlays only
    bool executable = false;             // activation is allowed (no blockers, evidence sufficient)
    bool needsConfirmation = false;      // EXPERIMENTAL: only after the user confirms
    std::string notExecutableReason;
    std::vector<std::string> blockedReasons;   // every rule that prevents activation
    // Separate answers (see compatibility::Assessment).
    std::string mappingRule;
    std::string mappingConfidence;
    std::string outcome;
    std::string category;
    std::string loading;
    std::string platform;
    std::size_t additions = 0;           // files the game does not have
    std::size_t replacements = 0;        // files that shadow game files
    std::size_t unknownTargets = 0;      // game listing incomplete or unavailable
    std::vector<std::string> installedConflicts;  // "Mod name: 3 files"
    std::vector<std::string> verification;        // what is checked after installing, and what cannot be
};

// What else the plan is based on, when known.
struct PlanContext {
    const ArchiveLayout* layout = nullptr;
    const compatibility::Assessment* assessment = nullptr;
    std::vector<Conflict> installedConflicts;
    std::uint64_t reserveBytes = 0;
};

// `hardLinks`: whether hard links work in Akeno's storage (from the system check); without
// them the overlay holds copies and needs as much space again. With an assessment, the plan is
// executable only when the assessment allows activation.
InstallPlan planInstall(const ModAnalysis& analysis, const AppPaths& paths, const std::string& titleId,
                        const std::string& downloadId, std::optional<bool> hardLinks = std::nullopt,
                        const PlanContext* context = nullptr);

}  // namespace akeno::mods
