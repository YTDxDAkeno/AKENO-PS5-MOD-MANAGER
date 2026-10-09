// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/compatibility/CompatibilityEngine.hpp"

#include <algorithm>
#include <set>

#include "akeno/core/Strings.hpp"
#include "akeno/unreal/CityHash.hpp"

namespace akeno::compatibility {

using mods::MappingConfidence;
using providers::CompatibilityStatus;

std::string_view toString(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::VerifiedPs5: return "VERIFIED_PS5";
        case Outcome::LikelyCompatible: return "LIKELY_COMPATIBLE";
        case Outcome::Experimental: return "EXPERIMENTAL";
        case Outcome::NeedsConversion: return "NEEDS_CONVERSION";
        case Outcome::RequiresUnsupportedLoader: return "REQUIRES_UNSUPPORTED_LOADER";
        case Outcome::Incompatible: return "INCOMPATIBLE";
        case Outcome::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<Outcome> parseOutcome(std::string_view text) noexcept {
    for (auto value : {Outcome::VerifiedPs5, Outcome::LikelyCompatible, Outcome::Experimental, Outcome::NeedsConversion,
                       Outcome::RequiresUnsupportedLoader, Outcome::Incompatible, Outcome::Unknown}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view toString(ModCategory category) noexcept {
    switch (category) {
        case ModCategory::PortableData: return "A";
        case ModCategory::PotentiallyPortable: return "B";
        case ModCategory::PcRuntimeDependent: return "C";
        case ModCategory::UnsupportedOrDangerous: return "D";
    }
    return "B";
}

std::string_view describe(ModCategory category) noexcept {
    switch (category) {
        case ModCategory::PortableData: return "portable data-only mod";
        case ModCategory::PotentiallyPortable: return "potentially portable: needs game-specific evidence";
        case ModCategory::PcRuntimeDependent: return "depends on a PC runtime or loader";
        case ModCategory::UnsupportedOrDangerous: return "unsupported or dangerous content";
    }
    return "";
}

std::optional<ModCategory> parseCategory(std::string_view text) noexcept {
    for (auto value : {ModCategory::PortableData, ModCategory::PotentiallyPortable, ModCategory::PcRuntimeDependent,
                       ModCategory::UnsupportedOrDangerous}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view toString(PlatformCompatibility value) noexcept {
    switch (value) {
        case PlatformCompatibility::Verified: return "verified";
        case PlatformCompatibility::Likely: return "likely";
        case PlatformCompatibility::Unknown: return "unknown";
        case PlatformCompatibility::NeedsConversion: return "needs-conversion";
        case PlatformCompatibility::Incompatible: return "incompatible";
    }
    return "unknown";
}

std::optional<PlatformCompatibility> parsePlatformCompatibility(std::string_view text) noexcept {
    for (auto value : {PlatformCompatibility::Verified, PlatformCompatibility::Likely, PlatformCompatibility::Unknown,
                       PlatformCompatibility::NeedsConversion, PlatformCompatibility::Incompatible}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view toString(LoadingSupport value) noexcept {
    switch (value) {
        case LoadingSupport::Verified: return "verified";
        case LoadingSupport::Expected: return "expected";
        case LoadingSupport::Unverified: return "unverified";
        case LoadingSupport::Unsupported: return "unsupported";
    }
    return "unverified";
}

std::optional<LoadingSupport> parseLoadingSupport(std::string_view text) noexcept {
    for (auto value : {LoadingSupport::Verified, LoadingSupport::Expected, LoadingSupport::Unverified, LoadingSupport::Unsupported}) {
        if (toString(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view toString(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::Supports: return "supports";
        case EvidenceKind::Against: return "against";
        case EvidenceKind::Limitation: return "limitation";
    }
    return "limitation";
}

std::string_view toString(EvidenceSource source) noexcept {
    switch (source) {
        case EvidenceSource::Archive: return "archive";
        case EvidenceSource::Container: return "container";
        case EvidenceSource::Game: return "game";
        case EvidenceSource::Catalogue: return "catalogue";
        case EvidenceSource::Adapter: return "adapter";
        case EvidenceSource::Rules: return "rules";
    }
    return "rules";
}

const Registry& Registry::builtin() {
    // Deliberately empty: an adapter or conversion is added only together with hardware
    // evidence for a specific title and version (docs/compatibility-engine.md).
    static const Registry registry;
    return registry;
}

std::string regionFromContentId(std::string_view contentId) {
    if (contentId.size() < 2) return {};
    const std::string prefix(contentId.substr(0, 2));
    if (prefix == "UP") return "Americas (UP)";
    if (prefix == "EP") return "Europe (EP)";
    if (prefix == "JP") return "Japan (JP)";
    if (prefix == "HP") return "Asia (HP)";
    return "unknown (" + prefix + ")";
}

namespace {

std::string parentOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string extensionOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string() : strings::toLowerAscii(name.substr(dot + 1));
}

std::string shortList(const std::vector<std::string>& items, std::size_t limit = 3) {
    std::string text;
    for (std::size_t i = 0; i < items.size() && i < limit; ++i) text += text.empty() ? items[i] : ", " + items[i];
    if (items.size() > limit) text += strings::concat(" and ", items.size() - limit, " more");
    return text;
}

int rank(MappingConfidence confidence) {
    switch (confidence) {
        case MappingConfidence::None: return 0;
        case MappingConfidence::Ambiguous: return 0;
        case MappingConfidence::Candidate: return 1;
        case MappingConfidence::Likely: return 2;
        case MappingConfidence::Established: return 3;
    }
    return 0;
}

}  // namespace

Assessment assess(const ModFacts& mod, const GameFacts& game, const Registry& registry) {
    Assessment a;
    auto add = [&](EvidenceKind kind, EvidenceSource source, std::string text) {
        a.evidence.push_back(Evidence{kind, source, std::move(text)});
    };
    a.region = regionFromContentId(game.contentId);
    if (mod.analysis == nullptr || mod.layout == nullptr) {
        a.blockedReasons.push_back("The mod has not been analysed.");
        a.summary = "UNKNOWN: not analysed.";
        return a;
    }
    const mods::ModAnalysis& analysis = *mod.analysis;
    const mods::ArchiveLayout& layout = *mod.layout;
    const unreal::UnrealAnalysis* ue = mod.unreal;
    const unreal::GameUnrealFacts* gu = game.unreal;
    const bool treeReadable = game.tree != nullptr && game.tree->available;
    const std::string versionText = game.version.empty() ? std::string("unknown version") : "version " + game.version;
    a.mappingConfidence = layout.confidence;

    // What it is.
    const bool modIsUnreal = ue != nullptr && (!ue->sets.empty() || !ue->looseAssets.empty());
    if (ue != nullptr && ue->detected) {
        a.engine = "Unreal Engine";
        a.modFormat = ue->format;
    } else if (analysis.engineHint == "Unity assets") {
        a.engine = "Unity";
    }
    if (a.modFormat.empty()) {
        a.modFormat = analysis.installCount == 0 ? std::string("nothing to install")
                                                 : strings::concat(analysis.installCount, " data file(s)");
    }
    for (const auto& loader : analysis.flags.loaders) a.pcDependencies.push_back(loader);
    if (ue != nullptr) {
        for (const auto& loader : ue->loaders) {
            if (std::find(a.pcDependencies.begin(), a.pcDependencies.end(), loader) == a.pcDependencies.end()) {
                a.pcDependencies.push_back(loader);
            }
        }
    }
    if (analysis.flags.windowsCode > 0) {
        a.pcDependencies.push_back(strings::concat("Windows programs or libraries (", analysis.flags.windowsCode, " files)"));
    }
    if (!a.region.empty()) add(EvidenceKind::Limitation, EvidenceSource::Game, "Region of the installed game: " + a.region + ".");

    // Category D: content Akeno never installs.
    bool categoryD = false;
    auto dangerous = [&](std::string reason) {
        categoryD = true;
        a.blockedReasons.push_back(reason);
        add(EvidenceKind::Against, EvidenceSource::Archive, std::move(reason));
    };
    if (analysis.flags.nativeCode > 0) {
        dangerous(strings::concat("Contains PS5 program code (", analysis.flags.nativeCode,
                                  " files). Akeno never installs or runs code."));
    }
    if (analysis.flags.fakelib) dangerous("Contains a fakelib folder, which ShadowMountPlus loads as system libraries.");
    if (analysis.flags.systemFolders) dangerous("Changes the game's system folders (sce_sys or sce_module).");
    if (analysis.flags.executableReplacement) dangerous("Would replace the game's executable.");
    if (analysis.flags.pathTooLong) dangerous("A file path is too long for ShadowMountPlus.");
    if (ue != nullptr) {
        for (const auto& issue : ue->issues) dangerous("Damaged or incomplete package set: " + issue);
    }

    // Category C: a PC runtime is required.
    bool categoryC = false;
    if (analysis.flags.windowsCode > 0) {
        categoryC = true;
        const std::string reason = strings::concat("Contains Windows programs or libraries (", analysis.flags.windowsCode,
                                                   " files). They need a Windows PC; renaming them does not change that.");
        a.blockedReasons.push_back(reason);
        add(EvidenceKind::Against, EvidenceSource::Archive, reason);
    }
    for (const auto& loader : a.pcDependencies) {
        if (strings::startsWith(loader, "Windows programs")) continue;
        categoryC = true;
        const std::string reason = "Needs " + loader + ", a PC-only mod loader. Copying its files into a PS5 overlay "
                                   "does not provide the loader.";
        a.blockedReasons.push_back(reason);
        add(EvidenceKind::Against, EvidenceSource::Archive, reason);
    }

    // Catalogue and provenance.
    bool incompatible = false;
    bool needsConversion = false;
    if (mod.curated) {
        switch (mod.catalogueStatus) {
            case CompatibilityStatus::Verified:
                add(EvidenceKind::Supports, EvidenceSource::Catalogue,
                    "The Akeno catalogue records this mod as verified on PS5 for the installed game " + versionText + ".");
                break;
            case CompatibilityStatus::Likely:
                add(EvidenceKind::Supports, EvidenceSource::Catalogue,
                    "The Akeno catalogue expects it to work; it is not verified on " + versionText + ".");
                break;
            case CompatibilityStatus::Experimental:
                add(EvidenceKind::Limitation, EvidenceSource::Catalogue, "The Akeno catalogue marks it experimental for " + versionText + ".");
                break;
            case CompatibilityStatus::PcOnly:
                needsConversion = true;
                add(EvidenceKind::Against, EvidenceSource::Catalogue, "The Akeno catalogue marks it as a PC mod that needs a PS5 port.");
                break;
            case CompatibilityStatus::Incompatible:
                incompatible = true;
                add(EvidenceKind::Against, EvidenceSource::Catalogue,
                    "The Akeno catalogue marks it incompatible with this game, region or version.");
                break;
            case CompatibilityStatus::Unknown:
                add(EvidenceKind::Limitation, EvidenceSource::Catalogue, "The Akeno catalogue has no compatibility record.");
                break;
        }
    } else if (mod.pcSource) {
        add(EvidenceKind::Limitation, EvidenceSource::Archive,
            "Made for the PC version of the game (" + mod.provider + "). Akeno has no record of it on PS5.");
    }

    // The installed game's engine and containers.
    if (modIsUnreal) {
        if (gu != nullptr && gu->probed) {
            add(EvidenceKind::Supports, EvidenceSource::Game, "The installed game is an Unreal Engine game: it has " + gu->paksDirectory + ".");
        } else if (treeReadable && game.tree->complete) {
            incompatible = true;
            add(EvidenceKind::Against, EvidenceSource::Game,
                "The installed game has no Unreal Content/Paks folder, so it does not use Unreal package files.");
        } else {
            add(EvidenceKind::Limitation, EvidenceSource::Game,
                "The installed game's files could not be read completely" +
                    std::string(gu != nullptr && !gu->reason.empty() ? " (" + gu->reason + ")" : "") +
                    ", so its engine and containers were not compared.");
        }
    }
    if (modIsUnreal && gu != nullptr && gu->probed) {
        if (ue->maxTocVersion > 0) {
            if (gu->maxTocVersion == 0) {
                if (gu->containersComplete) {
                    incompatible = true;
                    add(EvidenceKind::Against, EvidenceSource::Container,
                        "The game uses no IoStore (.utoc) containers, but this mod is one: the game's engine is not built to read it.");
                } else {
                    add(EvidenceKind::Limitation, EvidenceSource::Container, "The game's IoStore version could not be read.");
                }
            } else if (ue->maxTocVersion > gu->maxTocVersion) {
                incompatible = true;
                add(EvidenceKind::Against, EvidenceSource::Container,
                    strings::concat("The mod's IoStore version ", ue->maxTocVersion, " is newer than the game's (",
                                    gu->maxTocVersion, "): the game's engine cannot read it."));
            } else if (ue->maxTocVersion == gu->maxTocVersion) {
                add(EvidenceKind::Supports, EvidenceSource::Container,
                    strings::concat("The mod uses the same IoStore version as the game's own containers (", gu->maxTocVersion, ")."));
            } else {
                add(EvidenceKind::Limitation, EvidenceSource::Container,
                    strings::concat("The mod's IoStore version ", ue->maxTocVersion, " is older than the game's (",
                                    gu->maxTocVersion, "): it was made with an older engine build."));
            }
        }
        if (ue->maxPakVersion > 0 && gu->maxPakVersion > 0) {
            if (ue->maxPakVersion > gu->maxPakVersion) {
                incompatible = true;
                add(EvidenceKind::Against, EvidenceSource::Container,
                    strings::concat("The mod's .pak version ", ue->maxPakVersion, " is newer than the game's (",
                                    gu->maxPakVersion, ")."));
            } else if (ue->maxPakVersion == gu->maxPakVersion) {
                add(EvidenceKind::Supports, EvidenceSource::Container,
                    strings::concat("The mod's .pak version matches the game's (", gu->maxPakVersion, ")."));
            } else {
                add(EvidenceKind::Limitation, EvidenceSource::Container,
                    strings::concat("The mod's .pak version ", ue->maxPakVersion, " is older than the game's (",
                                    gu->maxPakVersion, ")."));
            }
        }
        if (gu->anySignedContainer || gu->signatureFiles) {
            const std::string text = "The game's own containers are signed. Akeno cannot sign mod containers, and a build "
                                     "that checks signatures refuses unsigned ones.";
            add(EvidenceKind::Against, EvidenceSource::Game, text);
            a.risks.push_back(text);
        } else if (gu->containersComplete) {
            add(EvidenceKind::Supports, EvidenceSource::Game, "The game's own containers are not signed.");
        }
        if (gu->anyEncrypted) {
            add(EvidenceKind::Limitation, EvidenceSource::Game,
                "The game's own containers are encrypted. That alone does not stop an unencrypted mod container from "
                "loading, but it is not verified.");
        }
        if (!ue->containedPackageIds.empty() && gu->packageIdsComplete) {
            std::size_t existing = 0;
            for (auto id : ue->containedPackageIds) existing += gu->hasPackage(id) ? 1 : 0;
            add(existing > 0 ? EvidenceKind::Supports : EvidenceKind::Limitation, EvidenceSource::Container,
                strings::concat(existing, " of ", ue->containedPackageIds.size(), " package(s) in the mod (",
                                shortList(ue->containedPackages, 2), ") exist in the installed PS5 game",
                                existing > 0 ? ": the mod replaces them." : "."));
        }
        if (!ue->importedPackages.empty()) {
            if (gu->packageIdsComplete) {
                std::vector<std::string> missing;
                std::size_t checked = 0;
                for (const auto& name : ue->importedPackages) {
                    if (!strings::startsWith(name, "/Game/")) continue;
                    const std::uint64_t id = unreal::packageIdFromName(name);
                    if (id == 0) continue;
                    ++checked;
                    const bool inMod = std::find(ue->containedPackageIds.begin(), ue->containedPackageIds.end(), id) !=
                                       ue->containedPackageIds.end();
                    if (!gu->hasPackage(id) && !inMod) missing.push_back(name);
                }
                if (!missing.empty()) {
                    incompatible = true;
                    add(EvidenceKind::Against, EvidenceSource::Container,
                        strings::concat(missing.size(), " of ", checked,
                                        " game packages the mod imports do not exist in the installed game (",
                                        shortList(missing, 2), "): it was built for another game version or build."));
                } else if (checked > 0) {
                    add(EvidenceKind::Supports, EvidenceSource::Container,
                        strings::concat("All ", checked, " game packages the mod imports exist in the installed PS5 game."));
                }
            } else {
                add(EvidenceKind::Limitation, EvidenceSource::Container,
                    "The game's package list could not be read completely, so the mod's imports were not compared.");
            }
        }
        if (!ue->looseAssets.empty() && gu->maxTocVersion > 0) {
            needsConversion = true;
            add(EvidenceKind::Against, EvidenceSource::Container,
                "Loose .uasset/.uexp files: this game loads packages from IoStore containers, so they would have to be "
                "packaged into a container first.");
        }
    }
    if (ue != nullptr) {
        for (const auto& set : ue->sets) {
            if (set.companionsVerified) {
                add(EvidenceKind::Supports, EvidenceSource::Container,
                    "The .utoc and .ucas of " + (set.directory.empty() ? set.stem : set.directory + "/" + set.stem) +
                        " belong together: every chunk hash matches.");
            }
        }
        if (ue->unversionedPackages) {
            const std::string text = "The packages are cooked with unversioned properties: they load correctly only in a "
                                     "game build whose classes match the build they were cooked for exactly. Akeno cannot "
                                     "check that on the console.";
            add(EvidenceKind::Limitation, EvidenceSource::Container, text);
            a.risks.push_back(text);
        }
        if (ue->compatibilityUnknown) {
            add(EvidenceKind::Limitation, EvidenceSource::Container,
                "COMPATIBILITY_UNKNOWN for parts that could not be parsed: " +
                    (ue->unknowns.empty() ? std::string("details in the report") : ue->unknowns.front()));
        }
        if (!ue->configFiles.empty()) {
            add(EvidenceKind::Limitation, EvidenceSource::Archive,
                "Unreal .ini files configure the PC build; the PS5 build reads its configuration from elsewhere.");
        }
        if (mod.pcSource) {
            for (const auto& package : ue->containedPackages) {
                const std::string name = package.substr(package.rfind('/') + 1);
                if (strings::startsWith(name, "BP_") || strings::startsWith(name, "ABP_")) {
                    a.risks.push_back("It replaces the Blueprint " + package + ". A whole gameplay Blueprint taken from "
                                      "another build can break or crash the game when it is loaded.");
                }
            }
        }
    }

    // Where the files go, and whether the game reads them there.
    const bool mapped = layout.hasMapping();
    const std::string target = layout.targetPrefix.empty() ? std::string("the game folder") : layout.targetPrefix;
    bool loadingVerified = false;
    bool pcAssetsVerified = false;
    bool modRecord = false;
    for (const auto& adapter : registry.adapters()) {
        if (!adapter->appliesTo(game.titleId)) continue;
        a.adapter = adapter->id();
        if (adapter->modVerified(mod, game)) modRecord = true;
        for (const auto& convention : adapter->conventions()) {
            if (!mapped) break;
            const std::string directory = strings::toLowerAscii(convention.directory);
            const bool covers = std::all_of(layout.mapping.begin(), layout.mapping.end(), [&](const auto& entry) {
                const std::string ext = extensionOf(entry.second);
                return strings::toLowerAscii(parentOf(entry.second)) == directory &&
                       std::find(convention.extensions.begin(), convention.extensions.end(), ext) != convention.extensions.end();
            });
            if (!covers) continue;
            const bool versionOk = !game.version.empty() &&
                                   std::find(convention.verifiedVersions.begin(), convention.verifiedVersions.end(),
                                             game.version) != convention.verifiedVersions.end();
            if (versionOk) {
                loadingVerified = true;
                pcAssetsVerified = pcAssetsVerified || convention.pcCookedAssetsVerified;
                add(EvidenceKind::Supports, EvidenceSource::Adapter,
                    "Game rule '" + adapter->id() + "': the game loads these files from " + convention.directory + " (" +
                        convention.evidence + ").");
            } else {
                std::string versions;
                for (const auto& v : convention.verifiedVersions) versions += versions.empty() ? v : ", " + v;
                add(EvidenceKind::Limitation, EvidenceSource::Adapter,
                    "Game rule '" + adapter->id() + "' covers " + convention.directory + " but was verified for " +
                        (versions.empty() ? std::string("no version") : "version " + versions) + ", not the installed " +
                        versionText + ".");
            }
        }
    }
    if (loadingVerified) {
        a.loading = LoadingSupport::Verified;
        if (rank(a.mappingConfidence) < rank(MappingConfidence::Established)) {
            a.mappingConfidence = MappingConfidence::Established;
            add(EvidenceKind::Supports, EvidenceSource::Adapter, "The installation path is established by a verified game rule.");
        }
    } else if (mod.curated && layout.confidence == MappingConfidence::Established) {
        a.loading = LoadingSupport::Expected;
        add(EvidenceKind::Supports, EvidenceSource::Catalogue, "The catalogue's installation layout was tested by its maintainers.");
    } else if (mapped && treeReadable &&
               std::all_of(layout.mapping.begin(), layout.mapping.end(), [&](const auto& entry) {
                   const auto* existing = game.tree->find(entry.second);
                   return existing != nullptr && !existing->directory;
               })) {
        a.loading = LoadingSupport::Expected;
        add(EvidenceKind::Supports, EvidenceSource::Game,
            "Every file replaces a game file at the same path: the game reads these paths.");
    } else if (mapped) {
        a.loading = LoadingSupport::Unverified;
        add(EvidenceKind::Limitation, EvidenceSource::Rules,
            "Nothing shows that the PS5 game (" + game.titleId + ", " + versionText + ") loads new files from " + target + ".");
    }
    if (layout.confidence == MappingConfidence::Candidate) {
        add(EvidenceKind::Limitation, EvidenceSource::Rules,
            "The installation path " + target + " is a candidate from an engine convention; the archive does not state it.");
    }

    // Platform compatibility of the content itself.
    if (incompatible || categoryD || categoryC) {
        a.platform = PlatformCompatibility::Incompatible;
    } else if (needsConversion) {
        a.platform = PlatformCompatibility::NeedsConversion;
    } else if (mod.curated) {
        a.platform = mod.catalogueStatus == CompatibilityStatus::Verified ? PlatformCompatibility::Verified
                     : mod.catalogueStatus == CompatibilityStatus::Likely  ? PlatformCompatibility::Likely
                                                                           : PlatformCompatibility::Unknown;
    } else if (modRecord) {
        a.platform = PlatformCompatibility::Verified;
    } else if (pcAssetsVerified) {
        a.platform = PlatformCompatibility::Likely;
    }

    // Conversions.
    if (needsConversion || mod.pcSource) {
        for (const auto& provider : registry.conversions()) {
            std::string reason;
            if (provider->canConvert(mod, game, &reason)) {
                a.conversions.push_back(provider->id() + ": " + provider->description());
            } else if (!reason.empty()) {
                add(EvidenceKind::Limitation, EvidenceSource::Rules, "Conversion '" + provider->id() + "' does not apply: " + reason);
            }
        }
        if (a.conversions.empty() && needsConversion) {
            add(EvidenceKind::Limitation, EvidenceSource::Rules, "No conversion for this game is available in this build.");
        }
    }

    // Outcome.
    if (categoryD) {
        a.outcome = Outcome::Incompatible;
    } else if (categoryC) {
        a.outcome = Outcome::RequiresUnsupportedLoader;
    } else if (a.platform == PlatformCompatibility::Incompatible) {
        a.outcome = Outcome::Incompatible;
    } else if (a.platform == PlatformCompatibility::NeedsConversion) {
        a.outcome = Outcome::NeedsConversion;
    } else if (mod.curated) {
        switch (mod.catalogueStatus) {
            case CompatibilityStatus::Verified:
                a.outcome = a.mappingConfidence == MappingConfidence::Established ? Outcome::VerifiedPs5 : Outcome::LikelyCompatible;
                break;
            case CompatibilityStatus::Likely: a.outcome = Outcome::LikelyCompatible; break;
            case CompatibilityStatus::Experimental: a.outcome = Outcome::Experimental; break;
            default: a.outcome = Outcome::Unknown; break;
        }
    } else if (modRecord && a.loading == LoadingSupport::Verified) {
        a.outcome = Outcome::VerifiedPs5;
    } else if (a.platform == PlatformCompatibility::Likely && a.loading == LoadingSupport::Verified) {
        a.outcome = Outcome::LikelyCompatible;
    } else if (rank(a.mappingConfidence) >= rank(MappingConfidence::Likely) && a.loading == LoadingSupport::Expected) {
        a.outcome = Outcome::Experimental;
    } else {
        a.outcome = Outcome::Unknown;
    }

    // Category.
    if (categoryD) {
        a.category = ModCategory::UnsupportedOrDangerous;
    } else if (categoryC) {
        a.category = ModCategory::PcRuntimeDependent;
    } else if ((a.outcome == Outcome::VerifiedPs5 || a.outcome == Outcome::LikelyCompatible) &&
               rank(a.mappingConfidence) >= rank(MappingConfidence::Likely)) {
        a.category = ModCategory::PortableData;
    } else {
        a.category = ModCategory::PotentiallyPortable;
    }

    // Activation: every rule that is not met is named.
    if (!categoryD && !categoryC) {
        if (!mapped) {
            a.blockedReasons.push_back("No reliable installation path: " +
                                       (layout.problems.empty() ? std::string("the archive layout is not understood.") : layout.problems.front()));
        } else if (a.mappingConfidence == MappingConfidence::Candidate) {
            a.blockedReasons.push_back("The installation path " + target + " is only a candidate: the archive does not "
                                       "state it, and no verified rule for this game confirms it.");
        }
        if (!mod.curated && !mod.pcSource) a.blockedReasons.push_back("The mod's source is unknown.");
        if (mod.pcSource && mapped && a.loading == LoadingSupport::Unverified) {
            a.blockedReasons.push_back("Nothing shows that this PS5 game (" + game.titleId + ", " + versionText +
                                       ") loads files from " + target + ".");
        }
        if (mod.pcSource && a.platform != PlatformCompatibility::Verified && a.platform != PlatformCompatibility::Likely) {
            a.blockedReasons.push_back("These files were made for the PC version. Their compatibility with the PS5 build (" +
                                       versionText + ") is " + std::string(toString(a.platform)) +
                                       ", so Akeno does not activate them.");
        }
        if (mod.curated && !mod.catalogueInstallable) {
            a.blockedReasons.push_back("The catalogue's rules do not allow installing it on this game version.");
        }
        if (a.outcome == Outcome::Incompatible || a.outcome == Outcome::NeedsConversion || a.outcome == Outcome::Unknown) {
            a.blockedReasons.push_back("Compatibility is " + std::string(toString(a.outcome)) + ".");
        }
        for (const auto& finding : analysis.findings) {
            if (finding.level == mods::FindingLevel::Blocker &&
                std::find(a.blockedReasons.begin(), a.blockedReasons.end(), finding.message) == a.blockedReasons.end()) {
                a.blockedReasons.push_back(finding.message);
            }
        }
    }
    a.activationAllowed = a.blockedReasons.empty();
    a.needsConfirmation = a.activationAllowed && a.outcome == Outcome::Experimental;

    for (const auto& conflict : mod.installedConflicts) {
        a.risks.push_back(strings::concat("Replaces ", conflict.count, " file(s) of the installed mod ", conflict.otherName,
                                          "; the mod later in the load order wins."));
    }
    a.summary = std::string(toString(a.outcome)) + " (category " + std::string(toString(a.category)) + "): path " +
                std::string(mods::toString(a.mappingConfidence)) + (mapped ? " (" + target + ")" : std::string()) +
                ", game loading " + std::string(toString(a.loading)) + ", platform " +
                std::string(toString(a.platform)) + (a.activationAllowed ? ", may be activated." : ", activation blocked.");
    return a;
}

}  // namespace akeno::compatibility
