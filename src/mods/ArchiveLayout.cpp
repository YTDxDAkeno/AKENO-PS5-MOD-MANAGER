// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/mods/ArchiveLayout.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "akeno/core/Strings.hpp"
#include "akeno/unreal/UnrealAnalyzer.hpp"

namespace akeno::mods {

namespace {

constexpr std::size_t kMaxPackagingFolders = 4;

std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    for (auto& part : strings::split(path, '/')) {
        if (!part.empty()) parts.push_back(std::move(part));
    }
    return parts;
}

std::string join(const std::vector<std::string>& parts, std::size_t from = 0, std::size_t to = std::string::npos) {
    std::string out;
    for (std::size_t i = from; i < std::min(to, parts.size()); ++i) out += out.empty() ? parts[i] : "/" + parts[i];
    return out;
}

bool under(std::string_view path, std::string_view root) {
    return root.empty() || (path.size() > root.size() && path.substr(0, root.size()) == root && path[root.size()] == '/');
}

std::string relativeTo(const std::string& path, const std::string& root) {
    return root.empty() ? path : path.substr(root.size() + 1);
}

bool isContainerFile(std::string_view path) {
    const std::string lower = strings::toLowerAscii(path);
    return strings::endsWith(lower, ".pak") || strings::endsWith(lower, ".utoc") || strings::endsWith(lower, ".ucas") ||
           strings::endsWith(lower, ".sig");
}

std::string inQuotes(std::string_view text) { return "'" + std::string(text) + "'"; }

// Index of the Unreal project folder in `parts` ("<Project>/Content/..."), if any.
std::optional<std::size_t> unrealAnchor(const std::vector<std::string>& parts, const std::string& path) {
    for (std::size_t i = 0; i + 2 < parts.size(); ++i) {
        if (!strings::equalsIgnoreCaseAscii(parts[i + 1], "content")) continue;
        const bool paks = i + 3 < parts.size() && strings::equalsIgnoreCaseAscii(parts[i + 2], "paks");
        if (paks || unreal::isUnrealContainerFile(path) || unreal::isUnrealAssetFile(path)) return i;
    }
    return std::nullopt;
}

void finishMapping(ArchiveLayout& layout) {
    // Two archive files must never land on the same game path.
    std::map<std::string, std::string> seen;
    for (const auto& [archivePath, installPath] : layout.mapping) {
        auto [it, inserted] = seen.emplace(strings::toLowerAscii(installPath), archivePath);
        if (inserted) continue;
        layout.problems.push_back("Both " + inQuotes(it->second) + " and " + inQuotes(archivePath) +
                                  " would be installed as " + inQuotes(installPath) + ".");
        layout.confidence = MappingConfidence::None;
    }
    if (layout.mapping.empty() && layout.confidence != MappingConfidence::Ambiguous) {
        layout.confidence = MappingConfidence::None;
        if (layout.problems.empty()) layout.problems.push_back("Nothing in the archive would be installed.");
    }
    if (!layout.hasMapping()) layout.mapping.clear();
}

}  // namespace

std::string_view toString(MappingConfidence confidence) noexcept {
    switch (confidence) {
        case MappingConfidence::None: return "none";
        case MappingConfidence::Ambiguous: return "ambiguous";
        case MappingConfidence::Candidate: return "candidate";
        case MappingConfidence::Likely: return "likely";
        case MappingConfidence::Established: return "established";
    }
    return "none";
}

std::optional<MappingConfidence> parseMappingConfidence(std::string_view text) noexcept {
    for (auto value : {MappingConfidence::None, MappingConfidence::Ambiguous, MappingConfidence::Candidate,
                       MappingConfidence::Likely, MappingConfidence::Established}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

ArchiveLayout analyzeLayout(const LayoutInput& input) {
    ArchiveLayout layout;
    std::set<std::string> topLevel;
    std::vector<const LayoutFile*> primary;
    for (const auto& file : input.files) {
        const auto parts = components(file.path);
        if (!parts.empty()) topLevel.insert(parts.front());
        if (file.kind == FileKind::Junk) {
            layout.ignored.emplace_back(file.path, "system leftover file");
        } else {
            primary.push_back(&file);
        }
    }
    layout.topLevel.assign(topLevel.begin(), topLevel.end());
    const games::GameTree* game = input.game != nullptr && input.game->available ? input.game : nullptr;

    // 1. A curated manifest decides; the game listing only adds observations.
    if (input.manifestArchiveRoot) {
        layout.rule = "manifest";
        layout.archiveRoot = *input.manifestArchiveRoot;
        layout.targetPrefix = input.manifestTargetPrefix;
        layout.confidence = MappingConfidence::Established;
        layout.evidence.push_back("The Akeno catalogue manifest maps archive folder " +
                                  inQuotes(layout.archiveRoot.empty() ? "(archive root)" : layout.archiveRoot) +
                                  " to game folder " + inQuotes(layout.targetPrefix.empty() ? "(game root)" : layout.targetPrefix) + ".");
        layout.packagingFolders = components(layout.archiveRoot);
        const std::string prefix = layout.targetPrefix.empty() ? std::string() : layout.targetPrefix + "/";
        for (const auto* file : primary) {
            if (!under(file->path, layout.archiveRoot)) {
                layout.ignored.emplace_back(file->path, "outside the folder the catalogue names");
                continue;
            }
            const std::string install = prefix + relativeTo(file->path, layout.archiveRoot);
            if (game != nullptr) {
                const std::string spelled = game->respell(install);
                if (spelled != install && layout.problems.size() < 8) {
                    layout.problems.push_back(inQuotes(install) + " differs only in upper/lower case from the game's " +
                                              inQuotes(spelled) + "; the catalogue's spelling is used.");
                }
            }
            layout.mapping.emplace_back(file->path, install);
        }
        finishMapping(layout);
        return layout;
    }

    // Documentation and preview images are not what decides the layout.
    std::vector<const LayoutFile*> core;
    for (const auto* file : primary) {
        if (file->kind != FileKind::Text && file->kind != FileKind::Image) core.push_back(file);
    }
    if (core.empty()) {
        for (const auto* file : primary) {
            if (file->kind != FileKind::Text) core.push_back(file);
        }
    }
    if (core.empty()) {
        layout.problems.push_back("The archive contains only documentation.");
        for (const auto* file : primary) layout.ignored.emplace_back(file->path, "documentation");
        finishMapping(layout);
        return layout;
    }
    std::vector<std::string> common = components(core.front()->path);
    common.pop_back();
    for (const auto* file : core) {
        auto parts = components(file->path);
        parts.pop_back();
        std::size_t shared = 0;
        while (shared < common.size() && shared < parts.size() && common[shared] == parts[shared]) ++shared;
        common.resize(shared);
    }

    // 2. An Unreal project layout inside the archive: "<Project>/Content/..." below packaging
    //    folders, or "Content/..." relative to the (implied) project folder.
    std::optional<std::size_t> anchorIndex;
    bool impliedProject = false;
    std::vector<std::string> anchorParts;
    std::string unanchored;
    std::set<std::string> projects;
    for (const auto* file : core) {
        const auto parts = components(file->path);
        auto index = unrealAnchor(parts, file->path);
        const bool implied = !index && parts.size() >= 2 && strings::equalsIgnoreCaseAscii(parts.front(), "content") &&
                             (unreal::isUnrealContainerFile(file->path) || unreal::isUnrealAssetFile(file->path));
        if (!index && !implied) {
            if (unanchored.empty()) unanchored = file->path;
            continue;
        }
        projects.insert(implied ? std::string("(project folder)") : strings::toLowerAscii(join(parts, 0, *index + 1)));
        if (!anchorIndex) {
            anchorIndex = implied ? 0 : *index;
            impliedProject = implied;
            anchorParts = parts;
        }
    }
    if (anchorIndex && impliedProject && unanchored.empty() && projects.size() == 1) {
        layout.rule = "unreal";
        std::string project;
        const std::string paks = game != nullptr ? unreal::findPaksDirectory(*game, &project) : std::string();
        if (paks.empty()) {
            layout.problems.push_back(game == nullptr
                                          ? "The archive starts at the Content folder, and the game's files could not be "
                                            "read to find its project folder."
                                          : "The archive starts at the Content folder, but the game has no single "
                                            "<project>/content/paks folder.");
            finishMapping(layout);
            return layout;
        }
        layout.confidence = MappingConfidence::Likely;
        layout.targetPrefix = project;
        layout.anchor = paks;
        layout.evidence.push_back("The archive starts at an Unreal Content folder; the game's only one is " + inQuotes(paks) + ".");
        for (const auto* file : primary) {
            const auto parts = components(file->path);
            if (!strings::equalsIgnoreCaseAscii(parts.front(), "content")) {
                layout.ignored.emplace_back(file->path, file->kind == FileKind::Text ? "documentation" : "outside the Content folder");
                continue;
            }
            layout.mapping.emplace_back(file->path, game->respell(project + "/" + file->path));
        }
        finishMapping(layout);
        return layout;
    }
    if (anchorIndex && !impliedProject) {
        layout.rule = "unreal";
        if (!unanchored.empty()) {
            layout.confidence = MappingConfidence::Ambiguous;
            layout.problems.push_back("Some files are inside an Unreal project folder and others are not (for example " +
                                      inQuotes(unanchored) + "), so the archive has no single layout.");
            finishMapping(layout);
            return layout;
        }
        if (projects.size() > 1) {
            layout.confidence = MappingConfidence::Ambiguous;
            std::string list;
            for (const auto& project : projects) list += list.empty() ? project : ", " + project;
            layout.problems.push_back("The archive contains several Unreal project folders (" + list +
                                      "), for example alternative versions of the mod. Akeno does not choose one.");
            finishMapping(layout);
            return layout;
        }
        const std::size_t i = *anchorIndex;
        const std::string project = anchorParts[i];
        layout.packagingFolders.assign(anchorParts.begin(), anchorParts.begin() + static_cast<std::ptrdiff_t>(i));
        layout.archiveRoot = join(anchorParts, 0, i);
        layout.anchor = join(anchorParts, i, std::min<std::size_t>(i + 3, anchorParts.size() - 1));
        if (!layout.packagingFolders.empty()) {
            layout.evidence.push_back("Folders above the Unreal project folder " + inQuotes(project) +
                                      " are packaging and are not installed: " + inQuotes(layout.archiveRoot) + ".");
        }
        std::string targetProject = project;
        if (game != nullptr) {
            std::string gameProject;
            const std::string paks = unreal::findPaksDirectory(*game, &gameProject);
            const games::GameTreeEntry* same = game->find(project);
            if (same != nullptr && same->directory) {
                targetProject = same->path;
                layout.confidence = MappingConfidence::Likely;
                layout.evidence.push_back("The archive's project folder " + inQuotes(project) + " is the game's " +
                                          inQuotes(same->path) + " folder.");
                if (const auto* content = game->find(project + "/content"); content != nullptr && content->directory) {
                    layout.evidence.push_back("The game has " + inQuotes(content->path) + ".");
                }
            } else if (!paks.empty()) {
                targetProject = gameProject;
                layout.confidence = MappingConfidence::Candidate;
                layout.evidence.push_back("The game's only Unreal content folder is " + inQuotes(paks) +
                                          "; the archive names its project " + inQuotes(project) + " instead of " +
                                          inQuotes(gameProject) + ".");
            } else {
                layout.confidence = MappingConfidence::None;
                layout.problems.push_back("The installed game has no " + inQuotes(project) +
                                          " folder and no Unreal Content/Paks folder.");
                finishMapping(layout);
                return layout;
            }
        } else {
            layout.confidence = MappingConfidence::Candidate;
            layout.problems.push_back("The game's files could not be read, so the project folder " + inQuotes(project) +
                                      " could not be confirmed.");
        }
        layout.targetPrefix = targetProject;
        for (const auto* file : primary) {
            if (!under(file->path, layout.archiveRoot)) {
                layout.ignored.emplace_back(file->path, file->kind == FileKind::Text ? "documentation" : "outside the mod's folder");
                continue;
            }
            auto rel = components(relativeTo(file->path, layout.archiveRoot));
            if (rel.size() < 2 || !strings::equalsIgnoreCaseAscii(rel.front(), project)) {
                layout.ignored.emplace_back(file->path, file->kind == FileKind::Text ? "documentation" : "outside the project folder");
                continue;
            }
            std::string install = targetProject + "/" + join(rel, 1);
            if (game != nullptr) install = game->respell(install);
            layout.mapping.emplace_back(file->path, install);
        }
        finishMapping(layout);
        return layout;
    }

    // 3. Folders that match the installed game's top-level folders.
    if (game != nullptr) {
        for (std::size_t k = 0; k <= std::min(kMaxPackagingFolders, common.size()); ++k) {
            // A leading folder that is itself a game folder is never treated as packaging.
            if (k == 1 && game->find(common.front()) != nullptr) break;
            const std::string root = join(common, 0, k);
            bool matches = true;
            bool deep = true;
            for (const auto* file : core) {
                const auto rel = components(relativeTo(file->path, root));
                if (rel.size() == 1) {
                    const auto* entry = game->find(rel.front());
                    matches = entry != nullptr && !entry->directory;
                } else {
                    const auto* entry = game->find(rel.front());
                    matches = entry != nullptr && entry->directory;
                    const auto* parent = game->find(join(rel, 0, rel.size() - 1));
                    if (parent == nullptr || !parent->directory) deep = false;
                }
                if (!matches) break;
            }
            if (!matches) continue;
            layout.rule = "game-root";
            layout.archiveRoot = root;
            layout.packagingFolders.assign(common.begin(), common.begin() + static_cast<std::ptrdiff_t>(k));
            layout.confidence = deep ? MappingConfidence::Likely : MappingConfidence::Candidate;
            if (k > 0) {
                layout.evidence.push_back("The leading folder(s) " + inQuotes(root) +
                                          " do not exist in the game and contain everything: packaging.");
            }
            std::set<std::string> matched;
            for (const auto* file : core) matched.insert(components(relativeTo(file->path, root)).front());
            for (const auto& name : matched) {
                const auto* entry = game->find(name);
                layout.evidence.push_back("The archive's " + inQuotes(name) + " is the game's " + inQuotes(entry->path) +
                                          (entry->directory ? " folder." : " file."));
            }
            layout.evidence.push_back(deep ? "Every file's folder already exists in the game."
                                           : "Some files create new folders inside the game's folders.");
            for (const auto* file : primary) {
                if (!under(file->path, root)) {
                    layout.ignored.emplace_back(file->path, file->kind == FileKind::Text ? "documentation" : "outside the mod's folder");
                    continue;
                }
                const auto rel = components(relativeTo(file->path, root));
                if (rel.size() == 1 && (file->kind == FileKind::Text || file->kind == FileKind::Image) &&
                    game->find(rel.front()) == nullptr) {
                    layout.ignored.emplace_back(file->path, file->kind == FileKind::Text ? "documentation" : "preview image");
                    continue;
                }
                layout.mapping.emplace_back(file->path, game->respell(join(rel)));
            }
            finishMapping(layout);
            return layout;
        }
    }

    // 4. A flat Unreal package set: the PC convention is <Project>/Content/Paks/~mods.
    const std::string folder = join(common);
    const bool flat = std::all_of(core.begin(), core.end(), [&](const LayoutFile* file) {
        return components(file->path).size() == common.size() + 1 && isContainerFile(file->path);
    });
    if (flat) {
        layout.rule = "unreal-flat";
        layout.archiveRoot = folder;
        layout.packagingFolders = common;
        std::string project;
        const std::string paks = game != nullptr ? unreal::findPaksDirectory(*game, &project) : std::string();
        if (paks.empty()) {
            layout.confidence = MappingConfidence::None;
            layout.problems.push_back(game == nullptr
                                          ? "The game's files could not be read, so its Content/Paks folder is unknown."
                                          : "The installed game has no single <project>/content/paks folder.");
            finishMapping(layout);
            return layout;
        }
        layout.confidence = MappingConfidence::Candidate;
        layout.anchor = paks;
        layout.targetPrefix = game->respell(paks + "/~mods");
        if (!folder.empty()) {
            layout.evidence.push_back("The folder " + inQuotes(folder) +
                                      " only wraps the package files (it is not a game folder): packaging.");
        }
        layout.evidence.push_back("The archive holds only Unreal package files and no game folders.");
        layout.evidence.push_back("The game has " + inQuotes(paks) + " (its Unreal package folder).");
        layout.problems.push_back("On PC, Unreal Engine also mounts packages found in Content/Paks/~mods. That this PS5 "
                                  "build does the same is not verified, so " + inQuotes(layout.targetPrefix) +
                                  " is only a candidate.");
        for (const auto* file : primary) {
            if (!under(file->path, folder) || components(file->path).size() != common.size() + 1 ||
                !isContainerFile(file->path)) {
                layout.ignored.emplace_back(file->path, file->kind == FileKind::Text ? "documentation" : "outside the package set");
                continue;
            }
            layout.mapping.emplace_back(file->path, layout.targetPrefix + "/" + components(file->path).back());
        }
        finishMapping(layout);
        return layout;
    }

    layout.rule = "none";
    std::string names;
    for (const auto& name : layout.topLevel) names += names.empty() ? name : ", " + name;
    layout.problems.push_back(game == nullptr
                                  ? "The game's files could not be read, so the archive layout (" + names +
                                        ") cannot be compared with the game."
                                  : "No folder of the archive (" + names + ") matches a folder of the installed game.");
    if (std::any_of(core.begin(), core.end(), [](const LayoutFile* f) { return unreal::isUnrealAssetFile(f->path); })) {
        layout.problems.push_back("Loose Unreal assets need their Content/... folder path to be placed.");
    }
    for (const auto* file : primary) layout.ignored.emplace_back(file->path, "no destination");
    finishMapping(layout);
    return layout;
}

}  // namespace akeno::mods
