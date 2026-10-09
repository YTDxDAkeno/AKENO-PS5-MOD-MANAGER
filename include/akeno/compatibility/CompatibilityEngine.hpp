// SPDX-License-Identifier: GPL-3.0-or-later
// The compatibility engine: what Akeno knows about a mod for one installed game, and whether
// that is enough to put it into the game's overlay.
//
// Four questions are kept apart, because a yes to one says nothing about the others:
//   mapping confidence      where the files go (mods::ArchiveLayout)
//   game loading support    whether this game reads files from there
//   platform compatibility  whether the content works on the PS5 build of this game version
//   activation              whether Akeno may install it into the overlay
//
// Facts come from the archive, the mod's containers, the installed game's files, the curated
// catalogue and game adapters (per-title knowledge backed by hardware evidence). Copying files,
// publishing an overlay or ShadowMountPlus mounting it are never evidence that a game loaded or
// accepted a mod. VERIFIED_PS5 needs a record for this mod on this exact title and version.
//
// Categories (docs/compatibility-engine.md):
//   A  portable data: format, path and loading are established for this game
//   B  potentially portable: needs game-specific evidence, validation or a conversion
//   C  needs a PC runtime: Windows code, UE4SS, Reloaded-II, script extenders, hooks
//   D  unsupported or dangerous: console code, system folders, damaged or incomplete containers
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/games/GameInfo.hpp"
#include "akeno/games/GameTree.hpp"
#include "akeno/mods/ArchiveLayout.hpp"
#include "akeno/mods/ModAnalyzer.hpp"
#include "akeno/providers/IModProvider.hpp"
#include "akeno/unreal/UnrealAnalyzer.hpp"

namespace akeno::compatibility {

enum class Outcome {
    VerifiedPs5,
    LikelyCompatible,
    Experimental,
    NeedsConversion,
    RequiresUnsupportedLoader,
    Incompatible,
    Unknown,
};
std::string_view toString(Outcome outcome) noexcept;  // "VERIFIED_PS5", ...
std::optional<Outcome> parseOutcome(std::string_view text) noexcept;

enum class ModCategory { PortableData, PotentiallyPortable, PcRuntimeDependent, UnsupportedOrDangerous };
std::string_view toString(ModCategory category) noexcept;  // "A".."D"
std::string_view describe(ModCategory category) noexcept;
std::optional<ModCategory> parseCategory(std::string_view text) noexcept;

enum class PlatformCompatibility { Verified, Likely, Unknown, NeedsConversion, Incompatible };
std::string_view toString(PlatformCompatibility value) noexcept;
std::optional<PlatformCompatibility> parsePlatformCompatibility(std::string_view text) noexcept;

enum class LoadingSupport {
    Verified,     // a game adapter records hardware evidence for this title and version
    Expected,     // curated catalogue entry, or files that replace the game's own at their paths
    Unverified,   // nothing shows that the game reads files from there
    Unsupported,  // known not to work
};
std::string_view toString(LoadingSupport value) noexcept;
std::optional<LoadingSupport> parseLoadingSupport(std::string_view text) noexcept;

enum class EvidenceKind { Supports, Against, Limitation };
// Report: the user's own test results on this console (see install::TestResult).
enum class EvidenceSource { Archive, Container, Game, Catalogue, Adapter, Rules, Report };
std::string_view toString(EvidenceKind kind) noexcept;
std::string_view toString(EvidenceSource source) noexcept;

struct Evidence {
    EvidenceKind kind = EvidenceKind::Limitation;
    EvidenceSource source = EvidenceSource::Rules;
    std::string text;
};

struct GameFacts {
    std::string titleId;
    std::string version;                     // installed content version, may be empty
    std::string contentId;
    games::SourceType sourceType = games::SourceType::Unknown;
    bool installedPkg = false;
    const games::GameTree* tree = nullptr;   // physical listing, when readable
    const unreal::GameUnrealFacts* unreal = nullptr;
};

struct ModFacts {
    std::string provider;                    // provider id
    std::string modId;
    std::string modVersion;
    bool curated = false;                    // Akeno catalogue: tested for PS5, manifest mapping
    bool pcSource = false;                   // Nexus Mods, GameBanana: made for the PC version
    providers::CompatibilityStatus catalogueStatus = providers::CompatibilityStatus::Unknown;
    bool catalogueInstallable = false;
    const mods::ModAnalysis* analysis = nullptr;
    const mods::ArchiveLayout* layout = nullptr;
    const unreal::UnrealAnalysis* unreal = nullptr;
    std::vector<mods::Conflict> installedConflicts;  // with mods already installed for the game
};

// Per-title knowledge. Add one only with real evidence; it is what turns a candidate path
// into an established one.
struct LoadingConvention {
    std::string directory;                   // game-relative, as spelled in the game
    std::vector<std::string> extensions;     // lower-case, without dot
    std::vector<std::string> verifiedVersions;  // game versions with hardware evidence
    std::string evidence;                    // what was observed, when, on which firmware
    bool pcCookedAssetsVerified = false;     // PC-cooked packages shown to load and work
};

class IGameAdapter {
public:
    virtual ~IGameAdapter() = default;
    virtual std::string id() const = 0;
    virtual bool appliesTo(std::string_view titleId) const = 0;
    virtual std::vector<LoadingConvention> conventions() const = 0;
    // Adapter: curated per-title knowledge. Report: the user's own test results on this console.
    virtual EvidenceSource evidenceSource() const { return EvidenceSource::Adapter; }
    // A hardware-verified record for this exact mod version on this title and game version.
    virtual bool modVerified(const ModFacts& mod, const GameFacts& game) const {
        (void)mod;
        (void)game;
        return false;
    }
    // A recorded problem with this exact mod version on this title and game version (for example
    // a crash the user reported after a test). Rules out another test.
    virtual std::optional<std::string> knownProblem(const ModFacts& mod, const GameFacts& game) const {
        (void)mod;
        (void)game;
        return std::nullopt;
    }
};

// A conversion of PC content into a form this PS5 game loads. A provider is offered only with
// a real implementation, its input requirements, validation and tests. None ships yet.
class IConversionProvider {
public:
    virtual ~IConversionProvider() = default;
    virtual std::string id() const = 0;
    virtual std::string description() const = 0;
    virtual bool canConvert(const ModFacts& mod, const GameFacts& game, std::string* reason) const = 0;
};

class Registry {
public:
    void addAdapter(std::shared_ptr<const IGameAdapter> adapter) { adapters_.push_back(std::move(adapter)); }
    void addConversion(std::shared_ptr<const IConversionProvider> provider) { conversions_.push_back(std::move(provider)); }
    const std::vector<std::shared_ptr<const IGameAdapter>>& adapters() const { return adapters_; }
    const std::vector<std::shared_ptr<const IConversionProvider>>& conversions() const { return conversions_; }
    // What ships with this build: no adapter and no conversion has hardware evidence yet.
    static const Registry& builtin();

private:
    std::vector<std::shared_ptr<const IGameAdapter>> adapters_;
    std::vector<std::shared_ptr<const IConversionProvider>> conversions_;
};

struct Assessment {
    Outcome outcome = Outcome::Unknown;
    ModCategory category = ModCategory::PotentiallyPortable;
    mods::MappingConfidence mappingConfidence = mods::MappingConfidence::None;
    PlatformCompatibility platform = PlatformCompatibility::Unknown;
    LoadingSupport loading = LoadingSupport::Unverified;
    bool activationAllowed = false;          // Akeno may install it into the overlay
    bool needsConfirmation = false;          // only after the user confirms (EXPERIMENTAL)
    std::vector<std::string> blockedReasons; // the exact rules that prevent activation
    std::vector<std::string> risks;
    std::vector<Evidence> evidence;
    std::string engine;                      // "Unreal Engine", "Unity", ...
    std::string modFormat;
    std::vector<std::string> pcDependencies;
    std::vector<std::string> conversions;    // offered conversion providers
    std::string adapter;                     // adapter that contributed, if any
    std::string region;                      // from the content id, informational
    std::string summary;

    // Test install. A PC mod that Akeno does not activate on its own may be installed as a test
    // after the user confirms its risks, when it consists only of data-only Unreal package sets
    // (no code, no loader, no textures, meshes, audio or shaders), every file is added to the
    // game's own package folder without replacing a game file, and every check that can be made
    // on the console passed. Not offered for curated or unknown sources.
    bool testInstallAvailable = false;
    std::vector<std::string> testBlockers;   // why a test is not offered
    std::vector<std::string> testRisks;      // shown before the user confirms a test
};

Assessment assess(const ModFacts& mod, const GameFacts& game, const Registry& registry = Registry::builtin());

// "UP0001-PPSA28000_00-..." -> "Americas (UP)"; empty when unknown.
std::string regionFromContentId(std::string_view contentId);

}  // namespace akeno::compatibility
