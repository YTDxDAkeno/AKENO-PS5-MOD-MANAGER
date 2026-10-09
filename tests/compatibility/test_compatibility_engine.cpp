// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/compatibility/CompatibilityEngine.hpp"

using namespace akeno;
using namespace akeno::compatibility;
using providers::CompatibilityStatus;

namespace {

games::GameTree gameTree(std::vector<std::string> paths) {
    games::GameTree tree;
    tree.available = true;
    tree.complete = true;
    for (auto& path : paths) {
        const bool folder = path.back() == '/';
        if (folder) path.pop_back();
        tree.entries.push_back({path, folder, 1});
    }
    tree.index();
    return tree;
}

// Runs layout, content analysis and assessment the way prepareMod does, without containers.
struct Case {
    std::vector<archives::ExtractedFile> files;
    games::GameTree tree = gameTree({"data/", "data/chara/", "data/chara/a.bin", "Game/", "Game/Content/", "Game/Content/Paks/"});
    unreal::UnrealAnalysis unreal;
    unreal::GameUnrealFacts gameUnreal;
    mods::ArchiveLayout layout;
    mods::ModAnalysis analysis;
    ModFacts mod;
    GameFacts game;

    Case& file(std::string path, std::string head = {}) {
        files.push_back({std::move(path), 10, std::string(64, 'a'), std::move(head)});
        return *this;
    }
    Assessment run(bool curated, CompatibilityStatus status, const Registry& registry = Registry::builtin(),
                   std::optional<std::string> manifestRoot = std::nullopt) {
        mods::LayoutInput layoutInput;
        for (const auto& f : files) layoutInput.files.push_back({f.path, mods::classifyFile(f.path, f.head)});
        layoutInput.game = &tree;
        if (curated) layoutInput.manifestArchiveRoot = manifestRoot.value_or("");
        layout = mods::analyzeLayout(layoutInput);
        mods::AnalysisInput input;
        input.files = files;
        input.catalogueStatus = status;
        input.catalogueInstallable = true;
        input.mapping = layout.mapping;
        analysis = mods::analyzeMod(input);
        mod.provider = curated ? "akeno-catalogue" : "nexus";
        mod.curated = curated;
        mod.pcSource = !curated;
        mod.catalogueStatus = status;
        mod.catalogueInstallable = true;
        mod.analysis = &analysis;
        mod.layout = &layout;
        mod.unreal = &unreal;
        game.titleId = "PPSA90001";
        game.version = "01.000.000";
        game.tree = &tree;
        game.unreal = &gameUnreal;
        return assess(mod, game, registry);
    }
};

bool mentions(const std::vector<std::string>& items, const std::string& text) {
    return std::any_of(items.begin(), items.end(), [&](const std::string& item) { return item.find(text) != std::string::npos; });
}

}  // namespace

TEST_CASE("curated mods follow the catalogue, and VERIFIED_PS5 needs the installed version") {
    Case verified;
    verified.file("data/chara/a.bin");
    auto a = verified.run(true, CompatibilityStatus::Verified);
    CHECK(a.outcome == Outcome::VerifiedPs5);
    CHECK(a.category == ModCategory::PortableData);
    CHECK(a.mappingConfidence == mods::MappingConfidence::Established);
    CHECK(a.activationAllowed);
    CHECK_FALSE(a.needsConfirmation);
    Case experimental;
    experimental.file("data/chara/a.bin");
    a = experimental.run(true, CompatibilityStatus::Experimental);
    CHECK(a.outcome == Outcome::Experimental);
    CHECK(a.activationAllowed);
    CHECK(a.needsConfirmation);
    Case unknown;
    unknown.file("data/chara/a.bin");
    a = unknown.run(true, CompatibilityStatus::Unknown);
    CHECK(a.outcome == Outcome::Unknown);
    CHECK_FALSE(a.activationAllowed);
}

TEST_CASE("title or region mismatches from the catalogue are INCOMPATIBLE") {
    Case c;
    c.file("data/chara/a.bin");
    const auto a = c.run(true, CompatibilityStatus::Incompatible);
    CHECK(a.outcome == Outcome::Incompatible);
    CHECK(a.platform == PlatformCompatibility::Incompatible);
    CHECK_FALSE(a.activationAllowed);
    Case pcOnly;
    pcOnly.file("data/chara/a.bin");
    CHECK(pcOnly.run(true, CompatibilityStatus::PcOnly).outcome == Outcome::NeedsConversion);
}

TEST_CASE("Windows code, UE4SS, Reloaded-II and dsts-loader are category C") {
    Case dll;
    dll.file("data/chara/a.bin").file("dinput8.dll", "MZ\x90");
    auto a = dll.run(false, CompatibilityStatus::Experimental);
    CHECK(a.category == ModCategory::PcRuntimeDependent);
    CHECK(a.outcome == Outcome::RequiresUnsupportedLoader);
    CHECK(mentions(a.blockedReasons, "renaming them does not change that"));
    Case renamed;  // a DLL renamed to look like data is still code
    renamed.file("data/chara/a.bin").file("data/chara/texture.bin", "MZ\x90");
    CHECK(renamed.run(false, CompatibilityStatus::Experimental).category == ModCategory::PcRuntimeDependent);
    Case reloaded;
    reloaded.file("Mod/ModConfig.json").file("Mod/data/chara/a.bin");
    a = reloaded.run(false, CompatibilityStatus::Experimental);
    CHECK(a.outcome == Outcome::RequiresUnsupportedLoader);
    CHECK(mentions(a.pcDependencies, "Reloaded-II"));
    Case dsts;
    dsts.file("dsts-loader/data/chara/a.bin");
    a = dsts.run(false, CompatibilityStatus::Experimental);
    CHECK(a.outcome == Outcome::RequiresUnsupportedLoader);
    CHECK(mentions(a.pcDependencies, "dsts-loader"));
    Case logic;
    logic.file("Game/Content/Paks/LogicMods/Cheat.pak");
    logic.unreal.detected = true;
    logic.unreal.loaders = {"UE4SS BPModLoader (LogicMods folder)"};
    a = logic.run(false, CompatibilityStatus::Experimental);
    CHECK(a.category == ModCategory::PcRuntimeDependent);
    CHECK(mentions(a.pcDependencies, "LogicMods"));
}

TEST_CASE("console code, system folders and damaged package sets are category D") {
    Case native;
    native.file("data/chara/a.bin").file("data/chara/lib.prx", "\x7F" "ELF");
    CHECK(native.run(true, CompatibilityStatus::Verified).category == ModCategory::UnsupportedOrDangerous);
    Case system;
    system.file("sce_sys/param.json");
    system.tree = gameTree({"sce_sys/", "sce_sys/param.json"});
    auto a = system.run(true, CompatibilityStatus::Verified);
    CHECK(a.category == ModCategory::UnsupportedOrDangerous);
    CHECK(a.outcome == Outcome::Incompatible);
    CHECK_FALSE(a.activationAllowed);
    Case fakelib;
    fakelib.file("fakelib/libSceAmpr.sprx");
    CHECK(fakelib.run(true, CompatibilityStatus::Verified).category == ModCategory::UnsupportedOrDangerous);
    Case damaged;
    damaged.file("Game/Content/Paks/~mods/x_P.pak");
    damaged.unreal.detected = true;
    damaged.unreal.issues = {"x_P.pak is damaged: No .pak footer was found."};
    a = damaged.run(true, CompatibilityStatus::Verified);
    CHECK(a.category == ModCategory::UnsupportedOrDangerous);
    CHECK(mentions(a.blockedReasons, "Damaged or incomplete package set"));
}

TEST_CASE("PC files without game evidence stay blocked, also when they replace game files at their paths") {
    Case replacing;
    replacing.file("data/chara/a.bin");
    auto a = replacing.run(false, CompatibilityStatus::Experimental);
    CHECK(a.loading == LoadingSupport::Expected);  // the game reads that path; the content is another matter
    CHECK(a.outcome == Outcome::Experimental);
    CHECK(a.platform == PlatformCompatibility::Unknown);
    CHECK_FALSE(a.activationAllowed);
    CHECK(mentions(a.blockedReasons, "made for the PC version"));
}

TEST_CASE("game rules apply only to their title and verified versions") {
    struct Rule final : IGameAdapter {
        std::string id() const override { return "rule"; }
        bool appliesTo(std::string_view titleId) const override { return titleId == "PPSA90001"; }
        std::vector<LoadingConvention> conventions() const override {
            return {{"data/chara", {"bin"}, {"01.000.000"}, "test", true}};
        }
    };
    Registry registry;
    registry.addAdapter(std::make_shared<Rule>());
    Case ok;
    ok.file("Wrapper/data/chara/new.bin");
    auto a = ok.run(false, CompatibilityStatus::Experimental, registry);
    CHECK(a.loading == LoadingSupport::Verified);
    CHECK(a.mappingConfidence == mods::MappingConfidence::Established);
    CHECK(a.outcome == Outcome::LikelyCompatible);
    CHECK(a.activationAllowed);
    CHECK(a.adapter == "rule");
    Case otherVersion;
    otherVersion.file("Wrapper/data/chara/new.bin");
    otherVersion.game.version = "01.001.000";
    otherVersion.mod.provider = "nexus";
    // run() resets the version, so assess directly after it.
    a = otherVersion.run(false, CompatibilityStatus::Experimental, registry);
    otherVersion.game.version = "01.001.000";
    a = assess(otherVersion.mod, otherVersion.game, registry);
    CHECK(a.loading == LoadingSupport::Unverified);
    CHECK_FALSE(a.activationAllowed);
    CHECK(std::any_of(a.evidence.begin(), a.evidence.end(), [](const Evidence& e) {
        return e.text.find("not the installed version 01.001.000") != std::string::npos;
    }));
    Case otherTitle;
    otherTitle.file("Wrapper/data/chara/new.bin");
    otherTitle.run(false, CompatibilityStatus::Experimental, registry);
    otherTitle.game.titleId = "PPSA90002";
    a = assess(otherTitle.mod, otherTitle.game, registry);
    CHECK(a.adapter.empty());
    CHECK_FALSE(a.activationAllowed);
}

TEST_CASE("Unreal mods for a game without Unreal containers, and loose assets for an IoStore game") {
    Case noEngine;
    noEngine.file("Game/Content/Paks/~mods/x_P.pak");
    noEngine.unreal.detected = true;
    noEngine.unreal.sets.push_back({});
    noEngine.tree = gameTree({"data/", "Game/"});
    auto a = noEngine.run(false, CompatibilityStatus::Experimental);
    CHECK(a.outcome == Outcome::Incompatible);
    Case loose;
    loose.file("Game/Content/Characters/Hero.uasset");
    loose.unreal.detected = true;
    loose.unreal.looseAssets = {"Game/Content/Characters/Hero.uasset"};
    loose.gameUnreal.probed = true;
    loose.gameUnreal.paksDirectory = "Game/Content/Paks";
    loose.gameUnreal.maxTocVersion = 8;
    a = loose.run(false, CompatibilityStatus::Experimental);
    CHECK(a.outcome == Outcome::NeedsConversion);
    CHECK(a.platform == PlatformCompatibility::NeedsConversion);
    CHECK(a.conversions.empty());
    CHECK(std::any_of(a.evidence.begin(), a.evidence.end(), [](const Evidence& e) {
        return e.text.find("No conversion for this game is available") != std::string::npos;
    }));
}

TEST_CASE("conversion providers are offered only when they declare they apply") {
    struct Converter final : IConversionProvider {
        std::string id() const override { return "test-converter"; }
        std::string description() const override { return "repackages loose assets (test)"; }
        bool canConvert(const ModFacts&, const GameFacts& game, std::string* reason) const override {
            if (game.titleId == "PPSA90001") return true;
            *reason = "only for PPSA90001";
            return false;
        }
    };
    Registry registry;
    registry.addConversion(std::make_shared<Converter>());
    Case loose;
    loose.file("Game/Content/Characters/Hero.uasset");
    loose.unreal.detected = true;
    loose.unreal.looseAssets = {"Game/Content/Characters/Hero.uasset"};
    loose.gameUnreal.probed = true;
    loose.gameUnreal.maxTocVersion = 8;
    const auto a = loose.run(false, CompatibilityStatus::Experimental, registry);
    REQUIRE(a.conversions.size() == 1);
    CHECK(a.conversions[0].find("test-converter") != std::string::npos);
    CHECK(a.outcome == Outcome::NeedsConversion);  // offering a conversion does not perform it
    CHECK_FALSE(a.activationAllowed);
    CHECK(Registry::builtin().adapters().empty());
    CHECK(Registry::builtin().conversions().empty());
}

TEST_CASE("regions come from the content id") {
    CHECK(regionFromContentId("UP0000-PPSA28000_00-0000000000000000") == "Americas (UP)");
    CHECK(regionFromContentId("EP0000-PPSA00000_00-0000000000000000") == "Europe (EP)");
    CHECK(regionFromContentId("JP0000-PPSA00000_00-0000000000000000") == "Japan (JP)");
    CHECK(regionFromContentId("") == "");
}
