// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/mods/ModPreparation.hpp"

#include <map>
#include <set>

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/providers/AkenoCatalogProvider.hpp"
#include "akeno/providers/GameBananaProvider.hpp"
#include "akeno/providers/NexusProvider.hpp"

namespace akeno::mods {

using logging::logger;

bool isCuratedProvider(std::string_view providerId) { return providerId == providers::kAkenoCatalogId; }

bool isPcProvider(std::string_view providerId) {
    return providerId == providers::kNexusProviderId || providerId == providers::kGameBananaProviderId;
}

namespace {

TargetState targetState(const games::GameTree& tree, const std::string& installPath) {
    if (!tree.available) return TargetState::Unknown;
    // A game file anywhere on the way makes the path impossible without hiding that file.
    std::string prefix;
    const auto parts = strings::split(installPath, '/');
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        prefix = prefix.empty() ? parts[i] : prefix + "/" + parts[i];
        const auto* entry = tree.find(prefix);
        if (entry != nullptr && !entry->directory) return TargetState::TypeConflict;
    }
    const auto* entry = tree.find(installPath);
    if (entry != nullptr) return entry->directory ? TargetState::TypeConflict : TargetState::Replaces;
    return tree.complete ? TargetState::New : TargetState::Unknown;
}

}  // namespace

Preparation prepareMod(const PreparationRequest& request) {
    Preparation p;
    const compatibility::Registry& registry = request.registry != nullptr ? *request.registry : compatibility::Registry::builtin();
    const GameContext& game = request.game;

    // 1. The installed game's physical files: only real folders, never runtime mounts.
    if (game.installedPkg) {
        p.gameTree.reason = "Installed packages have no physical folder Akeno can read.";
    } else if (game.sourceType == games::SourceType::Image) {
        p.gameTree.reason = "Disk-image games can only be read while mounted, and Akeno does not mount them.";
    } else if (game.physicalFolder.empty() || !games::isListableGameFolder(game.physicalFolder)) {
        p.gameTree.reason = "The game's physical folder is unknown.";
    } else {
        p.gameTree = games::probeGameTree(game.physicalFolder, request.treeLimits, request.cancel);
    }

    // 2. Where the files go.
    LayoutInput layoutInput;
    for (const auto& file : request.files) layoutInput.files.push_back({file.path, classifyFile(file.path, file.head)});
    layoutInput.game = &p.gameTree;
    if (request.curated) {
        layoutInput.manifestArchiveRoot = request.manifestArchiveRoot.value_or(std::string());
        layoutInput.manifestTargetPrefix = request.manifestTargetPrefix;
    }
    p.layout = analyzeLayout(layoutInput);
    if (request.paksFolderPlacement && p.layout.hasMapping() && p.gameTree.available) {
        // The package folder itself, where the game's own containers are: unlike ~mods, it needs
        // no search of subfolders. Only files directly in ~mods move; nothing else changes.
        const std::string paks = unreal::findPaksDirectory(p.gameTree);
        const std::string from = strings::toLowerAscii(paks) + "/~mods/";
        std::size_t moved = 0;
        std::size_t remaining = 0;
        for (auto& entry : p.layout.mapping) {
            const std::string folded = strings::toLowerAscii(entry.second);
            if (paks.empty() || !strings::startsWith(folded, from)) continue;
            if (entry.second.find('/', from.size()) != std::string::npos) {
                ++remaining;
                continue;
            }
            entry.second = paks + "/" + entry.second.substr(from.size());
            ++moved;
        }
        p.placementApplied = moved > 0 && remaining == 0;
        if (p.placementApplied) {
            if (strings::equalsIgnoreCaseAscii(p.layout.targetPrefix, paks + "/~mods")) p.layout.targetPrefix = paks;
            p.layout.evidence.push_back("Test placement chosen by you: directly in " + paks + " instead of " + paks + "/~mods.");
        }
    }

    // 3. Content checks on the mapped paths.
    AnalysisInput analysisInput;
    analysisInput.files = request.files;
    analysisInput.titleId = game.titleId;
    analysisInput.sourceType = game.sourceType;
    analysisInput.catalogueStatus = request.catalogueStatus;
    analysisInput.catalogueInstallable = request.catalogueInstallable;
    analysisInput.mapping = p.layout.mapping;
    p.analysis = analyzeMod(analysisInput);
    std::size_t typeConflicts = 0;
    std::string typeExample;
    for (auto& file : p.analysis.files) {
        if (file.installPath.empty()) continue;
        file.target = targetState(p.gameTree, file.installPath);
        if (file.target == TargetState::TypeConflict && typeConflicts++ == 0) typeExample = file.installPath;
    }
    if (typeConflicts > 0) {
        p.analysis.findings.insert(p.analysis.findings.begin(),
                                   AnalysisFinding{FindingLevel::Blocker,
                                                   strings::concat(typeConflicts, " file(s) would hide a folder or file of "
                                                                                  "the game that has the same path."),
                                                   typeExample});
        p.analysis.installable = false;
    }

    // 4. Unreal containers of the mod, then of the game.
    std::vector<unreal::ModFile> modFiles;
    for (const auto& file : request.files) modFiles.push_back({file.path, file.size, file.head});
    if (auto opener = unreal::DirectoryOpener::create(request.extractedRoot); opener) {
        p.unreal = unreal::analyzeUnreal(modFiles, *opener.value(), request.unrealLimits);
    } else {
        p.unreal = unreal::analyzeUnreal(modFiles, unreal::MemoryOpener{}, request.unrealLimits);
        p.unreal.compatibilityUnknown = true;
        p.unreal.unknowns.push_back("The extracted files could not be opened: " + opener.error().message);
    }
    if (p.unreal.detected && p.gameTree.available) {
        if (auto gameFiles = unreal::DirectoryOpener::create(game.physicalFolder); gameFiles) {
            p.gameUnreal = unreal::probeGameUnreal(p.gameTree, *gameFiles.value());
        } else {
            p.gameUnreal.reason = gameFiles.error().message;
        }
    } else if (p.unreal.detected) {
        p.gameUnreal.reason = p.gameTree.reason;
    }

    // 5. Overlap with mods already installed for the game.
    std::set<std::string> mine;
    for (const auto& file : p.analysis.files) {
        if (!file.installPath.empty()) mine.insert(strings::toLowerAscii(file.installPath));
    }
    for (const auto& other : request.installedMods) {
        Conflict conflict;
        conflict.otherId = other.id;
        conflict.otherName = other.name;
        for (const auto& path : other.installPaths) {
            if (mine.count(strings::toLowerAscii(path)) == 0) continue;
            if (conflict.paths.size() < 20) conflict.paths.push_back(path);
            ++conflict.count;
        }
        if (conflict.count > 0) p.installedConflicts.push_back(std::move(conflict));
    }

    // 6. The decision.
    compatibility::ModFacts facts;
    facts.provider = request.provider;
    facts.modId = request.modId;
    facts.modVersion = request.modVersion;
    facts.curated = request.curated;
    facts.pcSource = request.pcSource;
    facts.catalogueStatus = request.catalogueStatus;
    facts.catalogueInstallable = request.catalogueInstallable;
    facts.analysis = &p.analysis;
    facts.layout = &p.layout;
    facts.unreal = &p.unreal;
    facts.installedConflicts = p.installedConflicts;
    compatibility::GameFacts gameFacts;
    gameFacts.titleId = game.titleId;
    gameFacts.version = game.version;
    gameFacts.contentId = game.contentId;
    gameFacts.sourceType = game.sourceType.value_or(games::SourceType::Unknown);
    gameFacts.installedPkg = game.installedPkg;
    gameFacts.tree = &p.gameTree;
    gameFacts.unreal = &p.gameUnreal;
    p.assessment = compatibility::assess(facts, gameFacts, registry);
    logger().info("prepare", strings::concat(game.titleId, " ", request.provider, "/", request.modId, " ", request.modVersion,
                                             ": layout rule ", p.layout.rule, ", mapping ", toString(p.layout.confidence),
                                             p.layout.targetPrefix.empty() ? std::string() : " -> " + p.layout.targetPrefix,
                                             ", game listing ", p.gameTree.available ? (p.gameTree.complete ? "complete" : "partial") : "unavailable",
                                             ", ", p.assessment.summary));
    return p;
}

}  // namespace akeno::mods
