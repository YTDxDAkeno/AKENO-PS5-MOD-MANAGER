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

namespace akeno::mods {

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

struct AnalyzedFile {
    std::string archivePath;  // inside the archive
    std::string installPath;  // relative to the game's root; empty when not installed
    std::uint64_t size = 0;
    std::string sha256;
    FileKind kind = FileKind::Asset;
};

struct AnalysisInput {
    std::vector<archives::ExtractedFile> files;
    std::string archiveRoot;    // manifest installation.archiveRoot (validated relative path)
    std::string targetPrefix;   // manifest installation.targetPrefix
    std::string titleId;
    std::optional<games::SourceType> sourceType;  // of the installed game, when known
    providers::CompatibilityStatus catalogueStatus = providers::CompatibilityStatus::Unknown;
    bool catalogueInstallable = false;  // from the compatibility rules
};

struct ModAnalysis {
    std::vector<AnalyzedFile> files;          // every file in the archive
    std::vector<AnalysisFinding> findings;    // blockers first
    std::size_t installCount = 0;
    std::uint64_t installBytes = 0;
    std::string engineHint;                   // "Unreal Engine packages", ...
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

// The steps Phase 5 would take, described for the dry run.
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
    bool changesGameFiles = false;       // always false: overlays only
    bool executable = false;             // false in this version
    std::string notExecutableReason;
};
// `hardLinks`: whether hard links work in Akeno's storage (from the system check); without
// them the overlay holds copies and needs as much space again.
InstallPlan planInstall(const ModAnalysis& analysis, const AppPaths& paths, const std::string& titleId,
                        const std::string& downloadId, std::optional<bool> hardLinks = std::nullopt);

}  // namespace akeno::mods
