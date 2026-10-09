// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <map>

#include "TestSupport.hpp"
#include "akeno/mods/ArchiveLayout.hpp"

using namespace akeno;
using namespace akeno::mods;

namespace {

games::GameTree gameTree(std::vector<std::string> paths, bool complete = true) {
    games::GameTree tree;
    tree.root = "/data/homebrew/GAME";
    tree.available = true;
    tree.complete = complete;
    for (auto& path : paths) {
        const bool folder = !path.empty() && path.back() == '/';
        if (folder) path.pop_back();
        tree.entries.push_back({path, folder, folder ? 0u : 1u});
    }
    tree.index();
    return tree;
}

// The structure of the PPSA28000 folder as the 2026-10-08 diagnostic report listed it (subset).
games::GameTree dawnwalker() {
    return gameTree({"sce_sys/", "sce_sys/param.json", "eboot.bin", "dawnwalker/", "dawnwalker/content/",
                     "dawnwalker/content/movies/", "dawnwalker/content/movies/intro_cgi.bk2", "dawnwalker/content/paks/",
                     "dawnwalker/content/paks/dawnwalker-ps5.pak", "dawnwalker/content/paks/dawnwalker-ps5.ucas",
                     "dawnwalker/content/paks/dawnwalker-ps5.utoc"});
}

LayoutInput input(std::vector<std::string> paths, const games::GameTree* game) {
    LayoutInput in;
    for (const auto& path : paths) in.files.push_back({path, classifyFile(path, {})});
    in.game = game;
    return in;
}

std::map<std::string, std::string> mapping(const ArchiveLayout& layout) {
    return {layout.mapping.begin(), layout.mapping.end()};
}

}  // namespace

TEST_CASE("one packaging wrapper around a flat Unreal package set: candidate ~mods mapping") {
    const auto game = dawnwalker();
    const auto layout = analyzeLayout(input({"Better Carry Weight x10/00000000_BetterCarryWeightx10_P.pak",
                                             "Better Carry Weight x10/00000000_BetterCarryWeightx10_P.ucas",
                                             "Better Carry Weight x10/00000000_BetterCarryWeightx10_P.utoc"},
                                            &game));
    CHECK(layout.rule == "unreal-flat");
    CHECK(layout.confidence == MappingConfidence::Candidate);
    CHECK(layout.packagingFolders == std::vector<std::string>{"Better Carry Weight x10"});
    CHECK(layout.archiveRoot == "Better Carry Weight x10");
    CHECK(layout.targetPrefix == "dawnwalker/content/paks/~mods");
    CHECK(layout.anchor == "dawnwalker/content/paks");
    auto m = mapping(layout);
    CHECK(m["Better Carry Weight x10/00000000_BetterCarryWeightx10_P.pak"] ==
          "dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.pak");
    CHECK(m.size() == 3);
    REQUIRE_FALSE(layout.problems.empty());
    CHECK(layout.problems.back().find("not verified") != std::string::npos);
    // Without the game's files there is no destination at all.
    const auto unknown = analyzeLayout(input({"Mod/x_P.pak", "Mod/x_P.utoc", "Mod/x_P.ucas"}, nullptr));
    CHECK(unknown.confidence == MappingConfidence::None);
    CHECK(unknown.mapping.empty());
}

TEST_CASE("nested PC packaging folders above an Unreal project layout are removed, the layout kept") {
    const auto game = dawnwalker();
    const auto layout = analyzeLayout(input({"SomeMod/Files/Windows/Dawnwalker/Content/Paks/~mods/Mod_P.pak",
                                             "SomeMod/Files/Windows/Dawnwalker/Content/Paks/~mods/Mod_P.utoc",
                                             "SomeMod/Files/Windows/Dawnwalker/Content/Paks/~mods/Mod_P.ucas",
                                             "SomeMod/readme.txt"},
                                            &game));
    CHECK(layout.rule == "unreal");
    CHECK(layout.confidence == MappingConfidence::Likely);
    CHECK(layout.packagingFolders == std::vector<std::string>{"SomeMod", "Files", "Windows"});
    CHECK(layout.targetPrefix == "dawnwalker");
    auto m = mapping(layout);
    // Existing folders take the game's spelling; ~mods (new) keeps the archive's.
    CHECK(m["SomeMod/Files/Windows/Dawnwalker/Content/Paks/~mods/Mod_P.pak"] == "dawnwalker/content/paks/~mods/Mod_P.pak");
    CHECK(m.count("SomeMod/readme.txt") == 0);
    CHECK(layout.ignored.size() == 1);
}

TEST_CASE("legitimate game-root folders are never stripped") {
    const auto game = gameTree({"data/", "data/chara/", "data/chara/a.bin", "data/stage/", "eboot.bin"});
    // A single top-level folder that is a game folder stays, although it wraps everything.
    auto layout = analyzeLayout(input({"data/chara/a.bin", "data/chara/b.bin"}, &game));
    CHECK(layout.rule == "game-root");
    CHECK(layout.packagingFolders.empty());
    CHECK(layout.confidence == MappingConfidence::Likely);
    CHECK(mapping(layout)["data/chara/b.bin"] == "data/chara/b.bin");
    // Several game folders at the top.
    layout = analyzeLayout(input({"data/chara/a.bin", "data/stage/s.bin"}, &game));
    CHECK(layout.archiveRoot.empty());
    CHECK(layout.mapping.size() == 2);
}

TEST_CASE("removing a wrapper never flattens the folders below it") {
    const auto game = gameTree({"data/", "data/chara/", "data/stage/"});
    const auto layout = analyzeLayout(input({"Cool Mod v2/data/chara/a.bin", "Cool Mod v2/data/stage/s.bin",
                                             "Cool Mod v2/preview.png", "Cool Mod v2/README.md"},
                                            &game));
    CHECK(layout.rule == "game-root");
    CHECK(layout.packagingFolders == std::vector<std::string>{"Cool Mod v2"});
    auto m = mapping(layout);
    CHECK(m["Cool Mod v2/data/chara/a.bin"] == "data/chara/a.bin");
    CHECK(m["Cool Mod v2/data/stage/s.bin"] == "data/stage/s.bin");
    CHECK(m.count("Cool Mod v2/preview.png") == 0);
    CHECK(m.count("Cool Mod v2/README.md") == 0);
    // Two wrappers deep, still only leading folders.
    const auto deeper = analyzeLayout(input({"Pack/Option/data/chara/a.bin"}, &game));
    CHECK(mapping(deeper)["Pack/Option/data/chara/a.bin"] == "data/chara/a.bin");
    CHECK(deeper.packagingFolders == std::vector<std::string>{"Pack", "Option"});
}

TEST_CASE("alternative variants and mixed layouts are ambiguous, not guessed") {
    const auto game = dawnwalker();
    auto layout = analyzeLayout(input({"Option A/Dawnwalker/Content/Paks/~mods/a_P.pak",
                                       "Option B/Dawnwalker/Content/Paks/~mods/b_P.pak"},
                                      &game));
    CHECK(layout.confidence == MappingConfidence::Ambiguous);
    CHECK(layout.mapping.empty());
    layout = analyzeLayout(input({"Mod/Dawnwalker/Content/Paks/~mods/a_P.pak", "Mod/Binaries/extra.bin"}, &game));
    CHECK(layout.confidence == MappingConfidence::Ambiguous);
    layout = analyzeLayout(input({"Option A/a_P.pak", "Option B/b_P.pak"}, &game));
    CHECK(layout.confidence == MappingConfidence::None);
}

TEST_CASE("case collisions and conflicting targets invalidate a mapping") {
    const auto game = gameTree({"data/"});
    // Two archive files that the game's spelling folds onto the same path.
    auto layout = analyzeLayout(input({"Data/a.bin", "data/A.bin"}, &game));
    CHECK(layout.confidence == MappingConfidence::None);
    REQUIRE_FALSE(layout.problems.empty());
    CHECK(layout.problems[0].find("would be installed as") != std::string::npos);
    // Folder spelling follows the game, so differently cased folders merge cleanly.
    layout = analyzeLayout(input({"DATA/a.bin", "Data/b.bin"}, &game));
    CHECK(layout.confidence != MappingConfidence::None);
    for (const auto& [from, to] : layout.mapping) CHECK(to.substr(0, 5) == "data/");
}

TEST_CASE("a curated manifest decides the mapping and the game listing only adds observations") {
    const auto game = gameTree({"Content/", "Content/Paks/"});
    LayoutInput in = input({"Crimson/content/paks/~mods/c.pak", "README.txt"}, &game);
    in.manifestArchiveRoot = "Crimson";
    const auto layout = analyzeLayout(in);
    CHECK(layout.rule == "manifest");
    CHECK(layout.confidence == MappingConfidence::Established);
    CHECK(mapping(layout)["Crimson/content/paks/~mods/c.pak"] == "content/paks/~mods/c.pak");
    REQUIRE_FALSE(layout.problems.empty());  // the catalogue's spelling differs from the game's
    CHECK(layout.problems[0].find("upper/lower case") != std::string::npos);
}

TEST_CASE("project-relative Content layouts map into the game's project folder") {
    const auto game = dawnwalker();
    const auto layout = analyzeLayout(input({"Content/Paks/~mods/x_P.pak"}, &game));
    CHECK(layout.rule == "unreal");
    CHECK(layout.confidence == MappingConfidence::Likely);
    CHECK(mapping(layout)["Content/Paks/~mods/x_P.pak"] == "dawnwalker/content/paks/~mods/x_P.pak");
}

TEST_CASE("documentation-only archives and unmatched layouts have no mapping") {
    const auto game = dawnwalker();
    CHECK(analyzeLayout(input({"Mod/readme.txt"}, &game)).confidence == MappingConfidence::None);
    const auto layout = analyzeLayout(input({"Stuff/thing.bin"}, &game));
    CHECK(layout.confidence == MappingConfidence::None);
    REQUIRE_FALSE(layout.problems.empty());
    CHECK(layout.problems[0].find("No folder of the archive") != std::string::npos);
}
