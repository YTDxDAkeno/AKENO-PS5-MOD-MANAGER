// SPDX-License-Identifier: GPL-3.0-or-later
// Where the files of a mod archive belong in the game.
//
// Archives are packaged for people, not for Akeno: a mod often sits in a folder named after the
// mod ("Better Carry Weight x10/"), in PC staging folders ("Files/Windows/"), or next to readmes
// and preview images. Copying such a tree into the overlay as it is puts the files where no game
// looks. This analysis separates packaging from the paths the game uses, by deterministic rules:
//
//  1. manifest     A curated Akeno catalogue manifest names archiveRoot and targetPrefix.
//  2. unreal       The archive contains an Unreal project layout "<Project>/Content/...". Everything
//                  above the project folder is packaging; the project folder is matched to the
//                  game's own (case-insensitively).
//  3. game-root    After removing at most four leading folders that do not exist in the game, the
//                  archive's folders match the installed game's top-level folders.
//  4. unreal-flat  Only .pak/.utoc/.ucas files in one folder: on PC they go to
//                  <Project>/Content/Paks/~mods. This is a convention, so only a candidate.
//
// Only leading folders shared by every file are ever removed; nothing below the mapped root is
// flattened or renamed, except that folders which already exist in the game take the game's own
// spelling (PS5 folders are lower-case and the filesystem is case-sensitive). The result states
// how confident the mapping is; it never says whether the game will load the files or whether
// they work on PS5 (see compatibility::assess).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "akeno/games/GameTree.hpp"
#include "akeno/mods/ModAnalyzer.hpp"

namespace akeno::mods {

enum class MappingConfidence {
    None,         // no destination could be found
    Ambiguous,    // several incompatible destinations (for example alternative variants)
    Candidate,    // plausible by convention, not stated by the archive nor confirmed by the game
    Likely,       // the archive's own folders match folders of the installed game
    Established,  // stated by a curated manifest or a verified game adapter
};
std::string_view toString(MappingConfidence confidence) noexcept;
std::optional<MappingConfidence> parseMappingConfidence(std::string_view text) noexcept;

struct ArchiveLayout {
    std::string rule = "none";                // manifest, unreal, game-root, unreal-flat, none
    MappingConfidence confidence = MappingConfidence::None;
    std::vector<std::string> topLevel;        // top-level entries of the archive
    std::vector<std::string> packagingFolders;  // leading folders treated as packaging
    std::string archiveRoot;                  // archive folder whose contents are mapped
    std::string targetPrefix;                 // game-relative folder it maps to (proposed)
    std::string anchor;                       // recognised engine folder, e.g. "Dawnwalker/Content/Paks"
    std::vector<std::string> evidence;        // why this mapping
    std::vector<std::string> problems;        // why not better, or why none
    std::vector<std::pair<std::string, std::string>> mapping;  // archive path -> game-relative path
    std::vector<std::pair<std::string, std::string>> ignored;  // archive path -> reason not installed

    bool hasMapping() const { return confidence != MappingConfidence::None && confidence != MappingConfidence::Ambiguous; }
};

struct LayoutFile {
    std::string path;
    FileKind kind = FileKind::Asset;
};

struct LayoutInput {
    std::vector<LayoutFile> files;
    const games::GameTree* game = nullptr;        // the installed game's listing, when readable
    std::optional<std::string> manifestArchiveRoot;  // set for curated catalogue mods
    std::string manifestTargetPrefix;
};

ArchiveLayout analyzeLayout(const LayoutInput& input);

}  // namespace akeno::mods
