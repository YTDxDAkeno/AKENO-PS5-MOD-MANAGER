// SPDX-License-Identifier: GPL-3.0-or-later
// The on-console preparation of an extracted mod, shared by the check (Phase 4) and the
// installer (Phase 5), so the plan the user reviews is the plan that is executed:
//
//   list the installed game's physical folder (read-only)
//     -> archive layout and proposed mapping (ArchiveLayout)
//     -> content checks with that mapping (ModAnalysis)
//     -> Unreal container analysis of the extracted files
//     -> the game's own Unreal containers (headers and package ids only)
//     -> compatibility assessment and activation decision
//
// Nothing here writes anything. Game files are only opened read-only without following links.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/compatibility/CompatibilityEngine.hpp"
#include "akeno/core/Tasks.hpp"
#include "akeno/games/GameTree.hpp"
#include "akeno/mods/ArchiveLayout.hpp"
#include "akeno/mods/ModAnalyzer.hpp"
#include "akeno/unreal/UnrealAnalyzer.hpp"

namespace akeno::mods {

// The installed game a mod is prepared for.
struct GameContext {
    std::string titleId;
    std::string version;
    std::string contentId;
    std::optional<games::SourceType> sourceType;
    bool installedPkg = false;
    std::string physicalFolder;  // folder games: the source folder SMP reports; empty otherwise
};

struct InstalledModPaths {
    std::string id;
    std::string name;
    std::vector<std::string> installPaths;
};

struct PreparationRequest {
    std::vector<archives::ExtractedFile> files;
    std::filesystem::path extractedRoot;     // where `files` are (staging)
    GameContext game;
    std::string provider;
    std::string modId;
    std::string modVersion;
    bool curated = false;                    // Akeno catalogue
    bool pcSource = false;                   // Nexus Mods, GameBanana
    std::optional<std::string> manifestArchiveRoot;  // curated only
    std::string manifestTargetPrefix;
    providers::CompatibilityStatus catalogueStatus = providers::CompatibilityStatus::Unknown;
    bool catalogueInstallable = false;
    std::vector<InstalledModPaths> installedMods;  // other mods installed for the game
    const compatibility::Registry* registry = nullptr;  // default: the built-in registry
    // Test installs only: package files the layout puts into <paks>/~mods go into <paks> itself.
    bool paksFolderPlacement = false;
    const CancellationToken* cancel = nullptr;
    games::GameTreeLimits treeLimits{};
    unreal::UnrealLimits unrealLimits{};
};

struct Preparation {
    games::GameTree gameTree;
    ArchiveLayout layout;
    ModAnalysis analysis;
    unreal::UnrealAnalysis unreal;
    unreal::GameUnrealFacts gameUnreal;
    std::vector<Conflict> installedConflicts;
    compatibility::Assessment assessment;
    bool placementApplied = false;  // paksFolderPlacement moved every ~mods file
};

Preparation prepareMod(const PreparationRequest& request);

// True for the curated Akeno catalogue (its manifests map archives onto games).
bool isCuratedProvider(std::string_view providerId);
// Nexus Mods and GameBanana host mods made for the PC versions of games.
bool isPcProvider(std::string_view providerId);

}  // namespace akeno::mods
