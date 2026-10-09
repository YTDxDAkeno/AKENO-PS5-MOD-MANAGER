// SPDX-License-Identifier: GPL-3.0-or-later
// Unreal Engine mod analysis: which files form a package set, whether the set is complete and
// self-consistent, what it contains, which PC-only loaders it needs, and what the installed
// game's own containers look like. Facts only; the compatibility engine draws conclusions.
//
// A `.pak` + `.utoc` + `.ucas` set is linked only by its file names. Akeno additionally checks
// what the format allows: the .ucas length against the .utoc block table, the container id
// stored in the container-header chunk, and every chunk hash recomputed from the .ucas data.
// Matching extensions alone never count as evidence.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/games/GameTree.hpp"
#include "akeno/security/SafeOpen.hpp"
#include "akeno/unreal/UnrealFormats.hpp"

namespace akeno::unreal {

// Opens files below a folder (an extracted mod, or a game's physical folder) by relative path.
class FileOpener {
public:
    virtual ~FileOpener() = default;
    virtual Result<std::unique_ptr<ByteSource>> open(std::string_view relativePath) const = 0;
};

// Opens below a folder without following links in the folder's path or below it.
class DirectoryOpener final : public FileOpener {
public:
    static Result<std::unique_ptr<DirectoryOpener>> create(const std::filesystem::path& root);
    Result<std::unique_ptr<ByteSource>> open(std::string_view relativePath) const override;

private:
    explicit DirectoryOpener(security::UniqueFd root) : root_(std::move(root)) {}
    security::UniqueFd root_;
};

class MemoryOpener final : public FileOpener {
public:
    std::map<std::string, std::string> files;
    Result<std::unique_ptr<ByteSource>> open(std::string_view relativePath) const override;
};

struct ModFile {
    std::string path;          // archive-relative
    std::uint64_t size = 0;
    std::string head;          // first bytes
};

enum class PackageSetKind { IoStore, IncompleteIoStore, LegacyPak };
std::string_view toString(PackageSetKind kind) noexcept;

struct PackageSet {
    std::string directory;     // archive-relative folder of the set ("" for the archive root)
    std::string stem;          // file name without extension, as spelled
    PackageSetKind kind = PackageSetKind::LegacyPak;
    std::vector<std::string> members;  // archive paths
    bool hasPak = false, hasUtoc = false, hasUcas = false, hasSig = false;
    bool patchPriority = false;        // "_P" suffix: mounted after the game's own containers on PC
    std::optional<PakFile> pak;
    std::optional<IoStoreToc> toc;
    std::optional<ChunkVerification> chunks;
    std::optional<ContainerHeader> header;
    std::vector<ZenPackage> packages;
    std::vector<std::uint64_t> packageIds;  // chunk ids of the packages it provides
    bool companionsVerified = false;        // .utoc and .ucas proven to belong together
    std::vector<std::string> issues;        // incomplete or damaged: blocks installation
    std::vector<std::string> unknowns;      // what could not be checked
    std::vector<std::string> evidence;      // what was verified, and how
};

struct UnrealAnalysis {
    bool detected = false;
    std::string format;                      // "IoStore package set (.pak + .utoc + .ucas)", ...
    std::vector<PackageSet> sets;
    std::vector<std::string> looseAssets;    // .uasset/.umap/.uexp/.ubulk outside containers
    std::size_t unversionedLooseAssets = 0;
    std::vector<std::string> configFiles;    // Unreal .ini files
    std::vector<std::string> loaders;        // PC-only loaders the files are made for
    std::vector<std::string> modFolders;     // "~mods", "LogicMods" as found in the archive
    std::vector<std::string> containedPackages;  // packages the mod provides ("/Game/...")
    std::vector<std::uint64_t> containedPackageIds;
    std::vector<std::string> importedPackages;   // packages they import (bounded)
    int maxTocVersion = 0;
    int maxPakVersion = 0;
    bool anyEncrypted = false;
    bool anyCompressed = false;
    bool unversionedPackages = false;        // cooked without version information
    bool compatibilityUnknown = false;       // something could not be parsed reliably
    std::vector<std::string> issues;         // union of the sets' issues
    std::vector<std::string> unknowns;
    std::vector<std::string> evidence;
};

struct UnrealLimits {
    std::uint64_t maxHashBytes = 512ull * 1024 * 1024;     // chunk data re-hashed per check
    std::size_t maxPackages = 64;                           // package headers read per check
    std::uint64_t maxPackageBytes = 16ull * 1024 * 1024;    // per package chunk
    std::size_t maxImports = 2000;
};

bool isUnrealContainerFile(std::string_view path);
bool isUnrealAssetFile(std::string_view path);

UnrealAnalysis analyzeUnreal(const std::vector<ModFile>& files, const FileOpener& opener,
                             const UnrealLimits& limits = {});

// ------------------------------------------------------------------- the installed game

struct GameContainer {
    std::string path;          // game-relative
    std::string kind;          // "utoc" or "pak"
    ParseStatus status = ParseStatus::Malformed;
    std::string detail;
    int version = 0;
    std::uint8_t flags = 0;    // IoStore container flags
    std::vector<std::string> compressionMethods;
    std::uint64_t entries = 0;
    bool encryptedIndex = false;
};

struct GameUnrealFacts {
    bool probed = false;
    std::string reason;                // why nothing (or not everything) is known
    std::string projectDirectory;      // "dawnwalker", as spelled in the game
    std::string paksDirectory;         // "dawnwalker/content/paks"
    std::vector<GameContainer> containers;
    bool containersComplete = false;   // every .utoc/.pak below the paks folder was read
    std::vector<std::uint64_t> packageIds;   // sorted ExportBundleData ids of all containers
    bool packageIdsComplete = false;
    int maxTocVersion = 0;
    int maxPakVersion = 0;
    bool anySignedContainer = false;
    bool anyEncrypted = false;
    bool signatureFiles = false;       // .sig files next to the game's .pak files
    std::vector<std::string> compressionMethods;

    bool hasPackage(std::uint64_t id) const;
};

struct GameProbeLimits {
    std::size_t maxContainers = 128;
    std::uint64_t maxTocBytes = 512ull * 1024 * 1024;  // file size of one game .utoc
};

// "<project>/content/paks" in the game listing (case-insensitive). Empty when there is none or
// the choice is ambiguous. `project` receives the project folder as spelled.
std::string findPaksDirectory(const games::GameTree& tree, std::string* project = nullptr);

GameUnrealFacts probeGameUnreal(const games::GameTree& tree, const FileOpener& gameFiles,
                                const GameProbeLimits& limits = {});

}  // namespace akeno::unreal
