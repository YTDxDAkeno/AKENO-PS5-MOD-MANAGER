// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/mods/ModAnalyzer.hpp"

using namespace akeno;
using namespace akeno::mods;
using providers::CompatibilityStatus;

namespace {

archives::ExtractedFile file(std::string path, std::string head = "data", std::uint64_t size = 100) {
    archives::ExtractedFile f;
    f.path = std::move(path);
    f.head = std::move(head);
    f.size = size;
    f.sha256 = std::string(64, 'a');
    return f;
}

AnalysisInput input(std::vector<archives::ExtractedFile> files,
                    CompatibilityStatus status = CompatibilityStatus::Verified) {
    AnalysisInput in;
    in.files = std::move(files);
    in.titleId = "PPSA90001";
    in.catalogueStatus = status;
    in.catalogueInstallable = status == CompatibilityStatus::Verified || status == CompatibilityStatus::Likely ||
                              status == CompatibilityStatus::Experimental;
    return in;
}

bool hasFinding(const ModAnalysis& analysis, FindingLevel level, std::string_view text) {
    for (const auto& finding : analysis.findings) {
        if (finding.level == level && finding.message.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("files are classified by content first, then by name") {
    CHECK(classifyFile("Content/Paks/outfit.pak", "PAKDATA") == FileKind::Asset);
    CHECK(classifyFile("tools/innocent.png", "MZ\x90") == FileKind::WindowsCode);   // renamed executable
    CHECK(classifyFile("data/thing.bin", "\x7F" "ELF") == FileKind::NativeCode);
    CHECK(classifyFile("data/module.dat", std::string("\x4F\x15\x3D\x1D", 4)) == FileKind::NativeCode);  // SELF
    CHECK(classifyFile("eboot.bin", "x") == FileKind::NativeCode);
    CHECK(classifyFile("sce_module/libc.sprx", "x") == FileKind::NativeCode);
    CHECK(classifyFile("Binaries/Win64/dwmapi.DLL", "x") == FileKind::WindowsCode);
    CHECK(classifyFile("Mods/main.lua", "print") == FileKind::Script);
    CHECK(classifyFile("tool", "#!/bin/sh") == FileKind::Script);
    CHECK(classifyFile("extra/more.zip", "PK") == FileKind::Archive);
    CHECK(classifyFile("README", "Hello") == FileKind::Text);
    CHECK(classifyFile("textures/skin.dds", "DDS ") == FileKind::Image);
    CHECK(classifyFile("settings.ini", "[a]") == FileKind::Config);
    CHECK(classifyFile("__MACOSX/._a.pak", "x") == FileKind::Junk);
    CHECK(classifyFile("a/.DS_Store", "x") == FileKind::Junk);
}

TEST_CASE("a plain asset mod is installable and mapped through archiveRoot and targetPrefix") {
    AnalysisInput in = input({file("Crimson/Content/Paks/~mods/crimson.pak", "PAK", 5000), file("Crimson/config.ini", "[a]", 20),
                              file("README.txt", "Read me", 7), file("__MACOSX/._crimson.pak")});
    in.archiveRoot = "Crimson";
    in.targetPrefix = "ExampleBlade";
    in.sourceType = games::SourceType::Folder;
    ModAnalysis analysis = analyzeMod(in);
    CHECK(analysis.installable);
    CHECK_FALSE(analysis.hasBlockers());
    CHECK(analysis.status == CompatibilityStatus::Verified);
    CHECK(analysis.installCount == 2);
    CHECK(analysis.installBytes == 5020);
    CHECK(analysis.engineHint == "Unreal Engine packages");
    REQUIRE(analysis.files.size() == 4);
    CHECK(analysis.files[0].installPath == "ExampleBlade/Content/Paks/~mods/crimson.pak");
    CHECK(analysis.files[2].installPath.empty());  // README outside archiveRoot
    CHECK(analysis.files[3].installPath.empty());  // junk
    CHECK(hasFinding(analysis, FindingLevel::Info, "outside the mod folder"));
    CHECK(hasFinding(analysis, FindingLevel::Info, "__MACOSX"));
}

TEST_CASE("code is never installable, whatever the catalogue says") {
    auto pc = analyzeMod(input({file("Mod/Content/a.pak"), file("Mod/Binaries/Win64/dwmapi.dll", "MZ")}));
    CHECK_FALSE(pc.installable);
    CHECK(pc.status == CompatibilityStatus::PcOnly);
    CHECK(hasFinding(pc, FindingLevel::Blocker, "Windows programs"));

    auto ue4ss = analyzeMod(input({file("ue4ss/Mods/PhotoMode/Scripts/main.lua", "--")}));
    CHECK(ue4ss.status == CompatibilityStatus::PcOnly);
    CHECK(hasFinding(ue4ss, FindingLevel::Blocker, "UE4SS"));

    auto native = analyzeMod(input({file("data/plugin.dat", "\x7F" "ELF")}));
    CHECK(native.status == CompatibilityStatus::Incompatible);
    CHECK(hasFinding(native, FindingLevel::Blocker, "program code for the console"));

    auto fakelib = analyzeMod(input({file("fakelib/libSceAmpr.sprx", "x")}));
    CHECK(fakelib.status == CompatibilityStatus::Incompatible);
    CHECK(hasFinding(fakelib, FindingLevel::Blocker, "fakelib"));

    AnalysisInput prefixed = input({file("libs/readme.txt")});
    prefixed.targetPrefix = "FAKELIB2";  // case does not matter
    CHECK(hasFinding(analyzeMod(prefixed), FindingLevel::Blocker, "fakelib"));

    auto sceSys = analyzeMod(input({file("sce_sys/icon0.png", "\x89PNG")}));
    CHECK_FALSE(sceSys.installable);
    CHECK(hasFinding(sceSys, FindingLevel::Blocker, "sce_sys"));

    // Blockers come first.
    CHECK(pc.findings.front().level == FindingLevel::Blocker);
}

TEST_CASE("analysis never improves a label") {
    auto unknown = analyzeMod(input({file("a.pak")}, CompatibilityStatus::Unknown));
    CHECK(unknown.status == CompatibilityStatus::Unknown);
    CHECK_FALSE(unknown.installable);
    auto experimental = analyzeMod(input({file("a.pak")}, CompatibilityStatus::Experimental));
    CHECK(experimental.status == CompatibilityStatus::Experimental);
    CHECK(experimental.installable);
}

TEST_CASE("warnings for scripts, nested archives, case clashes and package limits") {
    std::vector<archives::ExtractedFile> files{file("Mods/init.lua", "--"), file("extra.zip", "PK"), file("Data/A.txt"),
                                              file("data/a.txt")};
    for (int i = 0; i < 300; ++i) files.push_back(file("Content/f" + std::to_string(i) + ".uasset"));
    AnalysisInput in = input(std::move(files));
    in.sourceType = games::SourceType::Pkg;
    ModAnalysis analysis = analyzeMod(in);
    CHECK(analysis.installable);  // warnings do not block
    CHECK(hasFinding(analysis, FindingLevel::Warning, "scripts"));
    CHECK(hasFinding(analysis, FindingLevel::Warning, "other archives"));
    CHECK(hasFinding(analysis, FindingLevel::Warning, "upper/lower case"));
    CHECK(hasFinding(analysis, FindingLevel::Warning, "at most 256"));

    AnalysisInput deep = input({file(std::string("a/").append(std::string(130, 'b')) )});
    std::string path;
    for (int i = 0; i < 70; ++i) path += "d/";
    deep.files = {file(path + "f.pak")};
    deep.sourceType = games::SourceType::Pkg;
    CHECK(hasFinding(analyzeMod(deep), FindingLevel::Blocker, "nested deeper"));
}

TEST_CASE("nothing to install is a blocker") {
    CHECK(hasFinding(analyzeMod(input({})), FindingLevel::Blocker, "nothing to install"));
    AnalysisInput wrongRoot = input({file("Other/a.pak")});
    wrongRoot.archiveRoot = "Mod";
    ModAnalysis analysis = analyzeMod(wrongRoot);
    CHECK_FALSE(analysis.installable);
    CHECK(hasFinding(analysis, FindingLevel::Blocker, "archiveRoot"));
}

TEST_CASE("conflicts are predicted without regard to case") {
    ModAnalysis a = analyzeMod(input({file("Content/Paks/~mods/outfit.pak"), file("Content/shared.ini")}));
    ModAnalysis b = analyzeMod(input({file("content/paks/~MODS/outfit.pak"), file("Content/other.ini")}));
    ModAnalysis c = analyzeMod(input({file("Content/unrelated.pak")}));
    auto conflicts = predictConflicts(a, {{"b", "Mod B", &b}, {"c", "Mod C", &c}});
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].otherName == "Mod B");
    CHECK(conflicts[0].count == 1);
    REQUIRE(conflicts[0].paths.size() == 1);
    CHECK(conflicts[0].paths[0] == "content/paks/~MODS/outfit.pak");
}

TEST_CASE("the dry-run plan describes an overlay and never the game files") {
    AppPaths paths{"/data/akeno-mod-manager"};
    ModAnalysis analysis = analyzeMod(input({file("Content/a.pak", "x", 1000), file("Content/b.pak", "x", 2000)}));
    InstallPlan plan = planInstall(analysis, paths, "PPSA90001", "0123456789abcdef");
    CHECK_FALSE(plan.changesGameFiles);
    CHECK_FALSE(plan.executable);
    CHECK(plan.notExecutableReason.find("Phase 5") != std::string::npos);
    CHECK(plan.files == 2);
    CHECK(plan.bytes == 3000);
    CHECK(plan.modStore == std::filesystem::path("/data/akeno-mod-manager/mods/PPSA90001/0123456789abcdef"));
    CHECK(plan.overlayNext == std::filesystem::path("/data/akeno-mod-manager/overlays/PPSA90001/overlay.next"));
    REQUIRE(plan.mapping.size() == 2);
    CHECK(plan.mapping[0].second == "/data/homebrew/backports/PPSA90001/Content/a.pak");
    CHECK(plan.steps.size() == 5);

    ModAnalysis blocked = analyzeMod(input({file("x.dll", "MZ")}));
    CHECK(planInstall(blocked, paths, "PPSA90001", "0123456789abcdef").notExecutableReason.find("problems") !=
          std::string::npos);
}

TEST_CASE("the plan uses copies when hard links do not work") {
    AppPaths paths{"/data/akeno-mod-manager"};
    ModAnalysis analysis = analyzeMod(input({file("Content/a.pak", "x", 3000)}));
    auto links = planInstall(analysis, paths, "PPSA90001", "0123456789abcdef", true);
    CHECK(links.overlayExtraBytes == 0);
    CHECK(links.steps[1].detail.find("hard links") != std::string::npos);
    auto copies = planInstall(analysis, paths, "PPSA90001", "0123456789abcdef", false);
    CHECK(copies.overlayExtraBytes == 3000);
    CHECK(copies.steps[1].detail.find("copies") != std::string::npos);
    CHECK(copies.steps[1].detail.find("3.0 KB") != std::string::npos);
    auto unknown = planInstall(analysis, paths, "PPSA90001", "0123456789abcdef");
    CHECK(unknown.overlayExtraBytes == 3000);
}
