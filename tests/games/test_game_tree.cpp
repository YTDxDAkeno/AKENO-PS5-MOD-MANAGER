// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <sys/stat.h>

#include "TestSupport.hpp"
#include "akeno/games/GameTree.hpp"

using namespace akeno;
namespace fs = std::filesystem;

TEST_CASE("the game listing is read-only, case-insensitive and skips links") {
    test::TempDir dir;
    const auto game = dir.path() / "game";
    fs::create_directories(game / "Dawnwalker/Content/Paks");
    test::writeText(game / "Dawnwalker/Content/Paks/main.pak", "pak");
    test::writeText(dir.path() / "outside.bin", "not part of the game");
    fs::create_symlink(dir.path() / "outside.bin", game / "link.bin");
    fs::create_directory_symlink(dir.path(), game / "up");
    REQUIRE(::mkfifo((game / "fifo").c_str(), 0600) == 0);
    const auto tree = games::probeGameTree(game);
    REQUIRE(tree.available);
    CHECK(tree.complete);
    CHECK(tree.entries.size() == 4);
    CHECK(tree.find("dawnwalker/content/paks/MAIN.PAK") != nullptr);
    CHECK(tree.find("dawnwalker/content/paks/main.pak")->size == 3);
    CHECK(tree.find("link.bin") == nullptr);
    CHECK(tree.find("up") == nullptr);
    CHECK(tree.respell("dawnwalker/CONTENT/paks/~mods/x.pak") == "Dawnwalker/Content/Paks/~mods/x.pak");
    CHECK(tree.knownAbsent("Dawnwalker/Content/Paks/~mods"));
    CHECK(tree.topLevel() == std::vector<std::string>{"Dawnwalker"});
}

TEST_CASE("the game listing refuses runtime paths and stops at its limits") {
    CHECK_FALSE(games::isListableGameFolder("/system_ex/app/PPSA28000"));
    CHECK_FALSE(games::isListableGameFolder("/mnt/shadowmnt/PPSA28000_1"));
    CHECK_FALSE(games::isListableGameFolder("/mnt/sandbox/PPSA28000_000/app0"));
    CHECK_FALSE(games::isListableGameFolder("/data/homebrew/backports/PPSA28000"));
    CHECK_FALSE(games::isListableGameFolder("/data/homebrew/../etc"));
    CHECK(games::isListableGameFolder("/data/homebrew/PPSA28000"));
    CHECK(games::isListableGameFolder("/mnt/usb0/homebrew/PPSA28000"));
    CHECK_FALSE(games::probeGameTree("/system_ex/app/PPSA28000").available);

    test::TempDir dir;
    for (int i = 0; i < 10; ++i) test::writeText(dir.path() / ("f" + std::to_string(i)), "x");
    games::GameTreeLimits limits;
    limits.maxEntries = 3;
    const auto partial = games::probeGameTree(dir.path(), limits);
    CHECK(partial.available);
    CHECK_FALSE(partial.complete);
    CHECK(partial.reason.find("listing limit") != std::string::npos);
    CHECK_FALSE(partial.knownAbsent("f9"));  // incomplete: absence is unknown
    fs::create_directory_symlink(dir.path(), dir.path().parent_path() / (dir.path().filename().string() + "-alias"));
    CHECK_FALSE(games::probeGameTree(dir.path().parent_path() / (dir.path().filename().string() + "-alias")).available);
    fs::remove(dir.path().parent_path() / (dir.path().filename().string() + "-alias"));
}
