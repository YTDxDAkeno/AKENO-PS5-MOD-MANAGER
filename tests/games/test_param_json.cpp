// SPDX-License-Identifier: GPL-3.0-or-later
// Game versions read from param.json when ShadowMountPlus does not report them (1.7beta4).
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/games/ParamJson.hpp"

using namespace akeno;
using namespace akeno::games;
namespace fs = std::filesystem;

namespace {

void writeParam(const fs::path& directory, const std::string& json) {
    fs::create_directories(directory);
    test::writeText(directory / "param.json", json);
}

GameInfo folderGame(const fs::path& path) {
    GameInfo game = test::makeGame("PPSA28000", "Example", "");
    game.sourceType = SourceType::Folder;
    game.installPath = path.string();
    game.runtimePath = path.string();
    return game;
}

}  // namespace

TEST_CASE("plausible game versions") {
    for (const char* good : {"01.005.000", "1.02", "1", "01.011"}) {
        CAPTURE(good);
        CHECK(isPlausibleGameVersion(good));
    }
    for (const char* bad : {"", ".1", "1.", "1..2", "v1.0", "1.0 beta", "../../etc", "01.005.000\n"}) {
        CAPTURE(bad);
        CHECK_FALSE(isPlausibleGameVersion(bad));
    }
    CHECK_FALSE(isPlausibleGameVersion(std::string(33, '1')));
}

TEST_CASE("contentVersion is taken from a PS5 param.json") {
    CHECK(contentVersionFromParamJson(R"({"titleId":"PPSA28000","contentVersion":"01.005.000","masterVersion":"01.00"})") ==
          std::optional<std::string>("01.005.000"));
    CHECK_FALSE(contentVersionFromParamJson(R"({"contentVersion":"latest"})"));
    CHECK_FALSE(contentVersionFromParamJson(R"({"contentVersion":1005})"));
    CHECK_FALSE(contentVersionFromParamJson(R"({"masterVersion":"01.00"})"));
    CHECK_FALSE(contentVersionFromParamJson("not json"));
    CHECK_FALSE(contentVersionFromParamJson("[]"));
}

TEST_CASE("versions are read from the game folder, the mounted path or appmeta") {
    test::TempDir dir;
    const fs::path game = dir.path() / "homebrew" / "Some Game 01.005";
    writeParam(game / "sce_sys", R"({"titleId":"PPSA28000","contentVersion":"01.005.000"})");
    auto found = readInstalledVersion(folderGame(game), dir.path() / "appmeta");
    REQUIRE(found);
    CHECK(found->version == "01.005.000");
    CHECK(found->source == game / "sce_sys" / "param.json");

    // A disk image is not a folder: its mounted path or the appmeta copy is used.
    GameInfo image = test::makeGame("PPSA28000", "Example", "");
    image.sourceType = SourceType::Image;
    image.installPath = (dir.path() / "game.ffpkg").string();
    image.runtimePath = (dir.path() / "not-mounted").string();
    writeParam(dir.path() / "appmeta" / "PPSA28000", R"({"contentVersion":"01.004.000"})");
    found = readInstalledVersion(image, dir.path() / "appmeta");
    REQUIRE(found);
    CHECK(found->version == "01.004.000");

    // Nothing usable anywhere: unknown, not guessed (folder names are never parsed).
    GameInfo missing = folderGame(dir.path() / "Another Game 02.000");
    missing.titleId = "PPSA11111";
    CHECK_FALSE(readInstalledVersion(missing, dir.path() / "appmeta"));
}

TEST_CASE("odd paths and oversized files are ignored") {
    test::TempDir dir;
    GameInfo relative = test::makeGame("PPSA28000", "Example", "");
    relative.sourceType = SourceType::Folder;
    relative.installPath = "relative/path";
    relative.runtimePath = "";
    CHECK_FALSE(readInstalledVersion(relative, dir.path() / "none"));

    const fs::path game = dir.path() / "big";
    fs::create_directories(game / "sce_sys");
    test::writeText(game / "sce_sys" / "param.json",
                    R"({"contentVersion":"01.000.000","pad":")" + std::string(kMaxParamJsonBytes, 'x') + "\"}");
    CHECK_FALSE(readInstalledVersion(folderGame(game), dir.path() / "none"));

    GameInfo badId = folderGame(dir.path() / "none");
    badId.titleId = "../../etc";
    writeParam(dir.path() / "etc", R"({"contentVersion":"9.9"})");
    CHECK_FALSE(readInstalledVersion(badId, dir.path() / "appmeta"));
}
