// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "UnrealTestSupport.hpp"
#include "akeno/unreal/UnrealAnalyzer.hpp"

using namespace akeno;
using namespace akeno::unreal;
namespace fs = std::filesystem;

namespace {

struct ModFiles {
    MemoryOpener opener;
    std::vector<ModFile> files;
    void add(const std::string& path, const std::string& data) {
        opener.files[path] = data;
        files.push_back({path, data.size(), data.substr(0, 64)});
    }
    void addSet(const std::string& base, const test::PackageSetFiles& set, bool pak = true, bool utoc = true, bool ucas = true) {
        if (pak) add(base + ".pak", set.pak);
        if (utoc) add(base + ".utoc", set.utoc);
        if (ucas) add(base + ".ucas", set.ucas);
    }
};

}  // namespace

TEST_CASE("a .pak, .utoc and .ucas of the same name form one verified package set") {
    ModFiles mod;
    mod.addSet("Wrapper/Mod_P", test::packageSet("/Game/Characters/BP_Hero", {"/Game/Characters/M_Hero"}));
    mod.add("Wrapper/readme.txt", "hello");
    const auto analysis = analyzeUnreal(mod.files, mod.opener);
    CHECK(analysis.detected);
    REQUIRE(analysis.sets.size() == 1);
    const auto& set = analysis.sets[0];
    CHECK(set.kind == PackageSetKind::IoStore);
    CHECK(set.directory == "Wrapper");
    CHECK(set.stem == "Mod_P");
    CHECK(set.patchPriority);
    CHECK(set.companionsVerified);
    CHECK(set.issues.empty());
    CHECK(analysis.format == "IoStore package set (.pak + .utoc + .ucas)");
    CHECK(analysis.maxTocVersion == 8);
    CHECK(analysis.maxPakVersion == 11);
    CHECK(analysis.containedPackages == std::vector<std::string>{"/Game/Characters/BP_Hero"});
    CHECK(analysis.importedPackages == std::vector<std::string>{"/Game/Characters/M_Hero"});
    CHECK(analysis.unversionedPackages);
    CHECK(analysis.issues.empty());
}

TEST_CASE("missing companion files are reported as incomplete package sets") {
    const auto set = test::packageSet("/Game/A/B", {});
    {
        ModFiles mod;
        mod.addSet("Mod_P", set, true, true, false);  // no .ucas
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        REQUIRE(analysis.sets.size() == 1);
        CHECK(analysis.sets[0].kind == PackageSetKind::IncompleteIoStore);
        REQUIRE_FALSE(analysis.issues.empty());
        CHECK(analysis.issues[0].find("no matching .ucas") != std::string::npos);
    }
    {
        ModFiles mod;
        mod.addSet("Mod_P", set, true, false, true);  // no .utoc
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        CHECK(analysis.issues[0].find("no matching .utoc") != std::string::npos);
    }
    {
        ModFiles mod;
        mod.addSet("Mod_P", set, false, true, true);  // no .pak
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        REQUIRE(analysis.issues.size() == 1);
        CHECK(analysis.issues[0].find("no companion .pak") != std::string::npos);
        CHECK(analysis.sets[0].companionsVerified);  // the .utoc and .ucas themselves still match
    }
    {
        // A .utoc and .ucas of different containers under the same name.
        ModFiles mod;
        const auto other = test::packageSet("/Game/Other/Thing", {}, 0x1234);
        mod.add("Mod_P.pak", set.pak);
        mod.add("Mod_P.utoc", set.utoc);
        mod.add("Mod_P.ucas", other.ucas + std::string(set.ucas.size(), '\0'));
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        REQUIRE_FALSE(analysis.issues.empty());
        CHECK_FALSE(analysis.sets[0].companionsVerified);
    }
    {
        ModFiles mod;
        mod.add("Mod_P.pak", set.pak);
        mod.add("Mod_P.utoc", set.utoc);
        mod.add("Mod_P.ucas", set.ucas.substr(0, 100));  // truncated
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        REQUIRE_FALSE(analysis.issues.empty());
        CHECK(analysis.issues[0].find("truncated") != std::string::npos);
    }
}

TEST_CASE("damaged and unknown container headers make the set damaged or its compatibility unknown") {
    const auto set = test::packageSet("/Game/A/B", {});
    {
        ModFiles mod;
        mod.add("Mod_P.pak", "this is not a pak file at all, just text that is long enough to have a footer region");
        mod.add("Mod_P.utoc", set.utoc);
        mod.add("Mod_P.ucas", set.ucas);
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        REQUIRE_FALSE(analysis.issues.empty());
        CHECK(analysis.issues[0].find(".pak is damaged") != std::string::npos);
    }
    {
        ModFiles mod;
        std::string newer = set.utoc;
        newer[16] = 9;  // a TOC version Akeno does not know
        mod.add("Mod_P.pak", set.pak);
        mod.add("Mod_P.utoc", newer);
        mod.add("Mod_P.ucas", set.ucas);
        const auto analysis = analyzeUnreal(mod.files, mod.opener);
        CHECK(analysis.issues.empty());
        CHECK(analysis.compatibilityUnknown);
        REQUIRE_FALSE(analysis.unknowns.empty());
        CHECK(analysis.unknowns[0].find("newer than Akeno knows") != std::string::npos);
    }
}

TEST_CASE("legacy paks, loose assets, configuration and PC loader folders are recognised") {
    ModFiles mod;
    mod.add("Game/Content/Paks/~mods/Legacy_P.pak", test::legacyPak({{"Game/Content/UI/W_Hud.uasset", "x"}}));
    test::Bytes uasset;
    uasset.u32(kPackageFileTag).i32(-8).i32(0).i32(0).i32(0).i32(0);
    mod.add("Game/Content/Characters/Hero.uasset", uasset.str());
    mod.add("Game/Content/Characters/Hero.uexp", "export data");
    mod.add("Game/Saved/Config/Windows/Engine.ini", "[/Script/Engine]");
    mod.add("Game/Content/Paks/LogicMods/Cheat.pak", test::legacyPak({{"Game/Content/Mods/Cheat.uasset", "x"}}));
    mod.add("Game/Binaries/Win64/ue4ss/UE4SS-settings.ini", "x");
    const auto analysis = analyzeUnreal(mod.files, mod.opener);
    CHECK(analysis.detected);
    CHECK(analysis.looseAssets.size() == 2);
    CHECK(analysis.unversionedLooseAssets == 1);
    CHECK(analysis.configFiles.size() == 1);
    CHECK(std::find(analysis.loaders.begin(), analysis.loaders.end(), "UE4SS") != analysis.loaders.end());
    CHECK(std::find(analysis.loaders.begin(), analysis.loaders.end(), "UE4SS BPModLoader (LogicMods folder)") != analysis.loaders.end());
    CHECK(std::find(analysis.modFolders.begin(), analysis.modFolders.end(), "~mods") != analysis.modFolders.end());
    CHECK(std::find(analysis.containedPackages.begin(), analysis.containedPackages.end(), "/Game/UI/W_Hud") != analysis.containedPackages.end());
    CHECK(std::find(analysis.containedPackages.begin(), analysis.containedPackages.end(), "/Game/Characters/Hero") != analysis.containedPackages.end());
    CHECK(analysis.format.find("pak file") != std::string::npos);
    CHECK(analysis.format.find("loose cooked assets") != std::string::npos);
}

TEST_CASE("the game's own containers are read from headers and package ids only") {
    test::TempDir dir;
    const auto game = dir.path() / "game";
    fs::create_directories(game / "dawnwalker/content/paks");
    fs::create_directories(game / "engine/content/paks");
    const auto set = test::packageSet("/Game/_Dawnwalker/Player/BP_PlayerCharacter", {});
    test::writeText(game / "dawnwalker/content/paks/dawnwalker-ps5.utoc", set.utoc);
    test::writeText(game / "dawnwalker/content/paks/dawnwalker-ps5.ucas", set.ucas);
    test::writeText(game / "dawnwalker/content/paks/dawnwalker-ps5.pak", set.pak);
    const auto tree = games::probeGameTree(game);
    REQUIRE(tree.complete);
    std::string project;
    CHECK(findPaksDirectory(tree, &project) == "dawnwalker/content/paks");
    CHECK(project == "dawnwalker");
    auto opener = DirectoryOpener::create(game);
    REQUIRE(opener.ok());
    const auto facts = probeGameUnreal(tree, *opener.value());
    REQUIRE(facts.probed);
    CHECK(facts.containersComplete);
    CHECK(facts.packageIdsComplete);
    CHECK(facts.maxTocVersion == 8);
    CHECK(facts.maxPakVersion == 11);
    CHECK_FALSE(facts.anySignedContainer);
    CHECK(facts.hasPackage(packageIdFromName("/Game/_Dawnwalker/Player/BP_PlayerCharacter")));
    CHECK_FALSE(facts.hasPackage(packageIdFromName("/Game/Nope")));
    CHECK(facts.containers.size() == 2);
}

TEST_CASE("files are opened below their folder without following links") {
    test::TempDir dir;
    fs::create_directories(dir.path() / "mod");
    test::writeText(dir.path() / "secret.pak", "outside");
    fs::create_symlink(dir.path() / "secret.pak", dir.path() / "mod" / "link.pak");
    fs::create_directory_symlink(dir.path(), dir.path() / "mod" / "up");
    auto opener = DirectoryOpener::create(dir.path() / "mod");
    REQUIRE(opener.ok());
    CHECK_FALSE(opener.value()->open("link.pak").ok());
    CHECK_FALSE(opener.value()->open("up/secret.pak").ok());
    CHECK_FALSE(opener.value()->open("../secret.pak").ok());
    CHECK_FALSE(DirectoryOpener::create(dir.path() / "mod" / "up").ok());
}
