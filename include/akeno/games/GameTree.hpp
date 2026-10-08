// SPDX-License-Identifier: GPL-3.0-or-later
// A read-only listing of an installed game's physical folder: relative paths, types and sizes.
// It is what mapping and compatibility checks compare a mod against ("does the game have a
// dawnwalker/content/paks folder?"). Nothing is hashed, written or followed: links and special
// files are skipped, nested filesystems are not entered, and the walk is bounded in entries,
// depth and time. An incomplete listing says so; a path missing from it is then unknown, not
// absent.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Tasks.hpp"

namespace akeno::games {

struct GameTreeEntry {
    std::string path;        // relative, '/'-separated, as spelled on disk
    bool directory = false;
    std::uint64_t size = 0;
};

struct GameTree {
    std::string root;        // the physical folder that was listed
    bool available = false;  // the folder could be opened
    bool complete = false;   // every entry was listed
    std::string reason;      // why it is unavailable or incomplete
    std::vector<GameTreeEntry> entries;  // sorted by path
    std::size_t caseCollisions = 0;      // paths that differ only in case (the first spelling wins)

    // Case-insensitive lookup of a relative path.
    const GameTreeEntry* find(std::string_view path) const;
    // True when `path` is known to be absent: the listing is complete and has no such entry.
    bool knownAbsent(std::string_view path) const { return complete && find(path) == nullptr; }
    // The game's own spelling of every existing leading component of `path`; components that do
    // not exist keep the given spelling. "Dawnwalker/Content/Paks/~mods" -> "dawnwalker/content/paks/~mods".
    std::string respell(std::string_view path) const;
    std::vector<std::string> topLevel() const;  // spelled as on disk
    // Builds the case-insensitive index; call after filling `entries` by hand (tests).
    void index();

private:
    std::map<std::string, std::size_t> lookup_;  // lower-case path -> entry
};

struct GameTreeLimits {
    std::size_t maxEntries = 200000;
    int maxDepth = 32;
    std::chrono::milliseconds maxDuration{15000};
};

// Lists `root`, an absolute physical folder. The caller decides which folders may be listed
// (never a runtime mount, sandbox or overlay path).
GameTree probeGameTree(const std::filesystem::path& root, const GameTreeLimits& limits = {},
                       const CancellationToken* cancel = nullptr);

// True for physical game folders Akeno may list: absolute, normalised, and not a runtime mount
// (/system_ex, /mnt/shadowmnt, /mnt/sandbox) or a backports folder.
bool isListableGameFolder(std::string_view path);

}  // namespace akeno::games
