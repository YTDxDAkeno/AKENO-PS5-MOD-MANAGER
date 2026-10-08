// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/unreal/UnrealAnalyzer.hpp"

#include <algorithm>
#include <cerrno>
#include <set>

#include "akeno/core/Strings.hpp"
#include "akeno/unreal/CityHash.hpp"

namespace akeno::unreal {

namespace {

std::string lowerExtension(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return strings::toLowerAscii(name.substr(dot + 1));
}

std::string directoryOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string stemOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

std::string joinPath(const std::string& directory, const std::string& name) {
    return directory.empty() ? name : directory + "/" + name;
}

std::string hexId(std::uint64_t id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kHex[id & 0xF];
        id >>= 4;
    }
    return text;
}

void addUnique(std::vector<std::string>& list, std::string value, std::size_t limit = 4000) {
    if (list.size() < limit && std::find(list.begin(), list.end(), value) == list.end()) list.push_back(std::move(value));
}

// "Dawnwalker/Content/_Dawnwalker/Player/BP.uasset" -> "/Game/_Dawnwalker/Player/BP".
std::optional<std::string> packageFromContentPath(std::string_view path) {
    std::string clean = stripMountPrefix(path);
    const auto parts = strings::split(clean, '/');
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!strings::equalsIgnoreCaseAscii(parts[i], "content")) continue;
        std::string package = "/Game";
        for (std::size_t j = i + 1; j < parts.size(); ++j) package += "/" + parts[j];
        const std::size_t dot = package.rfind('.');
        if (dot != std::string::npos && dot > package.rfind('/')) package.resize(dot);
        return package;
    }
    return std::nullopt;
}

void analyzeIoStore(PackageSet& set, const std::map<std::string, const ModFile*>& byLowerPath, const FileOpener& opener,
                    const UnrealLimits& limits, std::uint64_t& hashBudget, std::size_t& packageBudget) {
    const std::string base = joinPath(set.directory, set.stem);
    auto pathFor = [&](std::string_view extension) -> std::string {
        auto it = byLowerPath.find(strings::toLowerAscii(base) + "." + std::string(extension));
        return it == byLowerPath.end() ? std::string() : it->second->path;
    };
    if (!set.hasUtoc || !set.hasUcas) {
        set.kind = PackageSetKind::IncompleteIoStore;
        set.issues.push_back(set.hasUtoc ? base + ".utoc has no matching .ucas file: the container's data is missing."
                                         : base + ".ucas has no matching .utoc file: its table of contents is missing.");
    }
    if (!set.hasPak) {
        set.issues.push_back(base + " has no companion .pak file. Unreal Engine mounts an IoStore container through "
                                    "the .pak of the same name, so the container would never be loaded.");
    }
    if (set.hasPak) {
        auto source = opener.open(pathFor("pak"));
        if (!source) {
            set.unknowns.push_back("The .pak could not be read: " + source.error().message);
        } else {
            set.pak = parsePak(*source.value());
            const PakFile& pak = *set.pak;
            if (pak.status == ParseStatus::Malformed) {
                set.issues.push_back(base + ".pak is damaged: " + pak.detail);
            } else if (pak.status == ParseStatus::Unsupported) {
                set.unknowns.push_back(base + ".pak: " + pak.detail);
            } else {
                set.evidence.push_back(strings::concat(base, ".pak: version ", pak.version, ", mount point \"",
                                                       pak.mountPoint, "\", ", pak.entryCount, " files",
                                                       pak.indexHashVerified ? ", index SHA-1 verified" : ""));
            }
        }
    }
    if (!set.hasUtoc) return;
    auto tocSource = opener.open(pathFor("utoc"));
    if (!tocSource) {
        set.unknowns.push_back("The .utoc could not be read: " + tocSource.error().message);
        return;
    }
    set.toc = parseIoStoreToc(*tocSource.value());
    IoStoreToc& toc = *set.toc;
    if (toc.status == ParseStatus::Malformed) {
        set.issues.push_back(base + ".utoc is damaged: " + toc.detail);
        return;
    }
    if (toc.status == ParseStatus::Unsupported) {
        set.unknowns.push_back(base + ".utoc: " + toc.detail);
        return;
    }
    std::string flags;
    for (auto [bit, name] : {std::pair<int, const char*>{1, "compressed"}, {2, "encrypted"}, {4, "signed"}, {8, "indexed"}}) {
        if ((toc.flags & bit) != 0) flags += flags.empty() ? name : std::string(", ") + name;
    }
    set.evidence.push_back(strings::concat(base, ".utoc: IoStore version ", toc.version, ", container id ",
                                           hexId(toc.containerId), ", ", toc.chunks.size(), " chunks, flags: ",
                                           flags.empty() ? std::string("none") : flags));
    if (toc.encrypted()) {
        set.unknowns.push_back("The container is encrypted with a key Akeno does not have: its packages cannot be inspected.");
    }
    if (!toc.compressionMethods.empty()) {
        std::string methods;
        for (const auto& method : toc.compressionMethods) methods += methods.empty() ? method : ", " + method;
        set.unknowns.push_back("The container is compressed (" + methods + "); compressed chunks are not unpacked on the console.");
    }
    for (const auto& [file, chunk] : toc.files) {
        if (set.evidence.size() < 40) set.evidence.push_back("Directory index lists " + toc.mountPoint + file);
    }
    if (!set.hasUcas) return;
    auto casSource = opener.open(pathFor("ucas"));
    if (!casSource) {
        set.unknowns.push_back("The .ucas could not be read: " + casSource.error().message);
        return;
    }
    const ByteSource& cas = *casSource.value();
    if (toc.partitionCount <= 1 && cas.size() < toc.casBytesRequired) {
        set.issues.push_back(strings::concat(base, ".ucas has ", cas.size(), " bytes, but its .utoc needs ",
                                             toc.casBytesRequired, ": truncated or from another container."));
        return;
    }
    if (toc.partitionCount <= 1 && cas.size() > toc.casBytesRequired && !toc.encrypted()) {
        set.unknowns.push_back(strings::concat(base, ".ucas has ", cas.size() - toc.casBytesRequired,
                                               " bytes that its .utoc does not describe."));
    }
    if (!toc.encrypted()) {
        set.chunks = verifyChunks(toc, cas, hashBudget);
        const ChunkVerification& v = *set.chunks;
        std::uint64_t used = 0;
        for (const auto& chunk : toc.chunks) used += chunk.length;
        hashBudget -= std::min(hashBudget, used);
        if (v.mismatched > 0 && toc.version >= 8) {
            set.issues.push_back(strings::concat(v.mismatched, " chunk(s) in ", base,
                                                 ".ucas do not match the BLAKE3 hashes in the .utoc: the files are "
                                                 "damaged or do not belong together."));
        } else if (v.mismatched > 0) {
            set.unknowns.push_back(strings::concat(v.mismatched, " chunk hash(es) could not be confirmed (",
                                                   v.algorithm, ")."));
        }
        if (v.allVerified() && toc.chunks.size() == v.verified) {
            set.companionsVerified = true;
            set.evidence.push_back(strings::concat("All ", v.verified, " chunk hashes (", v.algorithm,
                                                   ") recomputed from the .ucas match the .utoc: the two files belong together."));
        } else if (v.skipped > 0) {
            set.unknowns.push_back(strings::concat(v.skipped, " chunk(s) were not hash-checked",
                                                   v.notes.empty() ? std::string(".") : " (" + v.notes.front() + ")."));
        }
    }
    for (std::size_t i = 0; i < toc.chunks.size(); ++i) {
        const IoChunk& chunk = toc.chunks[i];
        if (chunk.type == static_cast<std::uint8_t>(IoChunkType::ContainerHeader)) {
            auto data = readChunk(toc, i, cas, 16ull * 1024 * 1024);
            if (!data) {
                set.unknowns.push_back("The container header was not read: " + data.error().message);
                continue;
            }
            set.header = parseContainerHeader(data.value());
            if (set.header->status != ParseStatus::Parsed) {
                set.issues.push_back(base + ": the container header chunk is damaged.");
            } else if (set.header->containerId != toc.containerId || chunk.id != toc.containerId) {
                set.issues.push_back(base + ": the container header names a different container id than the .utoc.");
            } else {
                set.evidence.push_back(strings::concat("Container header (version ", set.header->version,
                                                       ") repeats container id ", hexId(toc.containerId), " and lists ",
                                                       set.header->packageIds.size(), " package(s)."));
                for (auto id : set.header->packageIds) set.packageIds.push_back(id);
            }
        } else if (chunk.type == static_cast<std::uint8_t>(IoChunkType::ExportBundleData)) {
            if (std::find(set.packageIds.begin(), set.packageIds.end(), chunk.id) == set.packageIds.end()) {
                set.packageIds.push_back(chunk.id);
            }
            if (packageBudget == 0) continue;
            --packageBudget;
            auto data = readChunk(toc, i, cas, limits.maxPackageBytes);
            if (!data) {
                set.unknowns.push_back("Package " + hexId(chunk.id) + " was not inspected: " + data.error().message);
                continue;
            }
            ZenPackage package = parseZenPackage(data.value());
            if (package.status != ParseStatus::Parsed) {
                set.unknowns.push_back("Package " + hexId(chunk.id) + " header: " + package.detail);
                continue;
            }
            const std::uint64_t expected = packageIdFromName(package.name);
            if (expected == chunk.id) {
                set.evidence.push_back(strings::concat("Package ", package.name, " (", package.layout, " header, ",
                                                       package.cooked() ? "cooked" : "not cooked",
                                                       package.unversioned() ? ", unversioned properties" : "",
                                                       ") has chunk id ", hexId(chunk.id),
                                                       " = CityHash64 of its name: identity confirmed."));
            } else {
                set.unknowns.push_back("Package " + package.name + " does not hash to its chunk id " + hexId(chunk.id) +
                                       "; its identity is not confirmed.");
            }
            set.packages.push_back(std::move(package));
        }
    }
}

}  // namespace

std::string_view toString(PackageSetKind kind) noexcept {
    switch (kind) {
        case PackageSetKind::IoStore: return "IoStore container (.pak + .utoc + .ucas)";
        case PackageSetKind::IncompleteIoStore: return "incomplete IoStore container";
        case PackageSetKind::LegacyPak: return "pak file";
    }
    return "pak file";
}

Result<std::unique_ptr<DirectoryOpener>> DirectoryOpener::create(const std::filesystem::path& root) {
    security::UniqueFd fd = security::openNoFollow(root, true);
    if (!fd.valid()) {
        return makeError(ErrorCode::IoError, "The folder cannot be opened.", root.string() + ": " + security::describeErrno(errno));
    }
    return std::unique_ptr<DirectoryOpener>(new DirectoryOpener(std::move(fd)));
}

Result<std::unique_ptr<ByteSource>> DirectoryOpener::open(std::string_view relativePath) const {
    security::UniqueFd fd = security::openBelow(root_.get(), relativePath, false);
    if (!fd.valid()) {
        return makeError(ErrorCode::IoError, "A file cannot be opened.",
                         std::string(relativePath) + ": " + security::describeErrno(errno));
    }
    auto source = FileSource::fromDescriptor(fd.release(), std::string(relativePath));
    if (!source) return std::move(source).error();
    return std::unique_ptr<ByteSource>(std::move(source).value());
}

Result<std::unique_ptr<ByteSource>> MemoryOpener::open(std::string_view relativePath) const {
    auto it = files.find(std::string(relativePath));
    if (it == files.end()) return makeError(ErrorCode::NotFound, "No such file.", std::string(relativePath));
    return std::unique_ptr<ByteSource>(std::make_unique<MemorySource>(it->second));
}

bool isUnrealContainerFile(std::string_view path) {
    const std::string ext = lowerExtension(path);
    return ext == "pak" || ext == "utoc" || ext == "ucas";
}

bool isUnrealAssetFile(std::string_view path) {
    const std::string ext = lowerExtension(path);
    return ext == "uasset" || ext == "uexp" || ext == "ubulk" || ext == "uptnl" || ext == "umap";
}

UnrealAnalysis analyzeUnreal(const std::vector<ModFile>& files, const FileOpener& opener, const UnrealLimits& limits) {
    UnrealAnalysis result;
    std::map<std::string, const ModFile*> byLowerPath;
    std::map<std::string, PackageSet> sets;  // lower-case dir/stem -> set
    for (const auto& file : files) {
        byLowerPath.emplace(strings::toLowerAscii(file.path), &file);
        const std::string lowerPath = strings::toLowerAscii(file.path);
        const std::string ext = lowerExtension(file.path);
        for (const auto& part : strings::split(lowerPath, '/')) {
            if (part == "~mods") addUnique(result.modFolders, "~mods");
            if (part == "logicmods") {
                addUnique(result.modFolders, "LogicMods");
                addUnique(result.loaders, "UE4SS BPModLoader (LogicMods folder)");
            }
            if (part == "ue4ss" || part == "ue4ss.dll" || part == "ue4ss-settings.ini") addUnique(result.loaders, "UE4SS");
        }
        if (ext == "lua" && lowerPath.find("/scripts/") != std::string::npos) addUnique(result.loaders, "UE4SS Lua mods");
        if (ext == "pak" || ext == "utoc" || ext == "ucas" || ext == "sig") {
            result.detected = true;
            const std::string key = strings::toLowerAscii(joinPath(directoryOf(file.path), stemOf(file.path)));
            PackageSet& set = sets[key];
            if (set.members.empty()) {
                set.directory = directoryOf(file.path);
                set.stem = stemOf(file.path);
                set.patchPriority = strings::endsWith(strings::toLowerAscii(set.stem), "_p");
            }
            set.members.push_back(file.path);
            if (ext == "pak") set.hasPak = true;
            if (ext == "utoc") set.hasUtoc = true;
            if (ext == "ucas") set.hasUcas = true;
            if (ext == "sig") set.hasSig = true;
        } else if (isUnrealAssetFile(file.path)) {
            result.detected = true;
            result.looseAssets.push_back(file.path);
            if (ext == "uasset" || ext == "umap") {
                const LegacyPackageSummary summary = parseLegacyPackageSummary(file.head);
                if (summary.status == ParseStatus::Parsed && summary.unversioned()) ++result.unversionedLooseAssets;
                if (summary.status != ParseStatus::Parsed) {
                    addUnique(result.unknowns, file.path + ": " + summary.detail);
                    result.compatibilityUnknown = true;
                }
                if (auto package = packageFromContentPath(file.path)) addUnique(result.containedPackages, *package);
            }
        } else if (ext == "ini" && (lowerPath.find("config/") != std::string::npos ||
                                    strings::startsWith(stemOf(lowerPath), "default") ||
                                    stemOf(lowerPath) == "engine" || stemOf(lowerPath) == "game" ||
                                    stemOf(lowerPath) == "input" || stemOf(lowerPath) == "gameusersettings")) {
            result.configFiles.push_back(file.path);
        }
    }
    if (!result.loaders.empty()) result.detected = true;

    std::uint64_t hashBudget = limits.maxHashBytes;
    std::size_t packageBudget = limits.maxPackages;
    std::size_t iostore = 0, legacy = 0, incomplete = 0;
    for (auto& [key, set] : sets) {
        if (set.hasUtoc || set.hasUcas) {
            set.kind = PackageSetKind::IoStore;
            analyzeIoStore(set, byLowerPath, opener, limits, hashBudget, packageBudget);
        } else if (set.hasPak) {
            set.kind = PackageSetKind::LegacyPak;
            std::string pakPath;
            for (const auto& member : set.members) {
                if (lowerExtension(member) == "pak") pakPath = member;
            }
            auto source = opener.open(pakPath);
            if (!source) {
                set.unknowns.push_back("The .pak could not be read: " + source.error().message);
            } else {
                set.pak = parsePak(*source.value());
                if (set.pak->status == ParseStatus::Malformed) {
                    set.issues.push_back(joinPath(set.directory, set.stem) + ".pak is damaged: " + set.pak->detail);
                } else if (set.pak->status == ParseStatus::Unsupported) {
                    set.unknowns.push_back(joinPath(set.directory, set.stem) + ".pak: " + set.pak->detail);
                } else {
                    set.evidence.push_back(strings::concat(joinPath(set.directory, set.stem), ".pak: version ",
                                                           set.pak->version, ", mount point \"", set.pak->mountPoint,
                                                           "\", ", set.pak->entryCount, " files",
                                                           set.pak->indexHashVerified ? ", index SHA-1 verified" : ""));
                    for (const auto& path : set.pak->files) {
                        if (auto package = packageFromContentPath(path)) addUnique(result.containedPackages, *package);
                    }
                }
            }
        } else {
            set.kind = PackageSetKind::LegacyPak;
            set.issues.push_back(joinPath(set.directory, set.stem) + ".sig has no .pak file to sign.");
        }
        if (set.kind == PackageSetKind::IncompleteIoStore) ++incomplete;
        else if (set.kind == PackageSetKind::IoStore) ++iostore;
        else ++legacy;
        if (set.pak && set.pak->status != ParseStatus::Malformed) result.maxPakVersion = std::max(result.maxPakVersion, set.pak->version);
        if (set.toc && set.toc->version > 0) result.maxTocVersion = std::max(result.maxTocVersion, set.toc->version);
        if (set.toc && set.toc->encrypted()) result.anyEncrypted = true;
        if (set.pak && set.pak->encryptedIndex) result.anyEncrypted = true;
        if (set.toc && !set.toc->compressionMethods.empty()) result.anyCompressed = true;
        for (const auto& package : set.packages) {
            addUnique(result.containedPackages, package.name);
            if (package.unversioned()) result.unversionedPackages = true;
            for (const auto& import : package.importedPackages) {
                if (result.importedPackages.size() < limits.maxImports) addUnique(result.importedPackages, import, limits.maxImports);
            }
            if (!package.importsParsed) addUnique(result.unknowns, "The imports of " + package.name + " could not be read.");
        }
        for (auto id : set.packageIds) {
            if (std::find(result.containedPackageIds.begin(), result.containedPackageIds.end(), id) == result.containedPackageIds.end()) {
                result.containedPackageIds.push_back(id);
            }
        }
        for (const auto& issue : set.issues) result.issues.push_back(issue);
        for (const auto& unknown : set.unknowns) addUnique(result.unknowns, unknown);
        for (const auto& item : set.evidence) result.evidence.push_back(item);
        if (!set.unknowns.empty()) result.compatibilityUnknown = true;
        result.sets.push_back(std::move(set));
    }
    std::vector<std::string> parts;
    if (iostore > 0) parts.push_back(strings::concat("IoStore package set", iostore > 1 ? "s" : "", " (.pak + .utoc + .ucas)"));
    if (incomplete > 0) parts.push_back("incomplete IoStore files");
    if (legacy > 0) parts.push_back(strings::concat(legacy, legacy == 1 ? " pak file" : " pak files"));
    if (!result.looseAssets.empty()) parts.push_back("loose cooked assets (.uasset/.uexp)");
    if (!result.configFiles.empty()) parts.push_back("Unreal configuration files");
    for (const auto& part : parts) result.format += result.format.empty() ? part : ", " + part;
    if (result.detected && result.format.empty()) result.format = "Unreal Engine loader files";
    return result;
}

// ------------------------------------------------------------------- the installed game

bool GameUnrealFacts::hasPackage(std::uint64_t id) const {
    return std::binary_search(packageIds.begin(), packageIds.end(), id);
}

std::string findPaksDirectory(const games::GameTree& tree, std::string* project) {
    std::vector<const games::GameTreeEntry*> candidates;
    for (const auto& entry : tree.entries) {
        if (!entry.directory) continue;
        const auto parts = strings::split(entry.path, '/');
        if (parts.size() != 3 || !strings::equalsIgnoreCaseAscii(parts[1], "content") ||
            !strings::equalsIgnoreCaseAscii(parts[2], "paks") || strings::equalsIgnoreCaseAscii(parts[0], "engine")) {
            continue;
        }
        candidates.push_back(&entry);
    }
    if (candidates.size() != 1) return {};
    if (project != nullptr) *project = candidates.front()->path.substr(0, candidates.front()->path.find('/'));
    return candidates.front()->path;
}

GameUnrealFacts probeGameUnreal(const games::GameTree& tree, const FileOpener& gameFiles, const GameProbeLimits& limits) {
    GameUnrealFacts facts;
    if (!tree.available) {
        facts.reason = "The game's files could not be listed: " + tree.reason;
        return facts;
    }
    facts.paksDirectory = findPaksDirectory(tree, &facts.projectDirectory);
    if (facts.paksDirectory.empty()) {
        facts.reason = tree.complete ? "The game has no single <project>/content/paks folder."
                                     : "No <project>/content/paks folder was found in the incomplete listing.";
        return facts;
    }
    facts.probed = true;
    facts.containersComplete = tree.complete;
    facts.packageIdsComplete = tree.complete;
    const std::string prefix = strings::toLowerAscii(facts.paksDirectory) + "/";
    for (const auto& entry : tree.entries) {
        if (entry.directory || !strings::startsWith(strings::toLowerAscii(entry.path), prefix)) continue;
        const std::string ext = lowerExtension(entry.path);
        if (ext == "sig") facts.signatureFiles = true;
        if (ext != "utoc" && ext != "pak") continue;
        if (facts.containers.size() >= limits.maxContainers) {
            facts.containersComplete = false;
            facts.packageIdsComplete = false;
            break;
        }
        GameContainer container;
        container.path = entry.path;
        container.kind = ext;
        auto source = gameFiles.open(entry.path);
        if (!source) {
            container.detail = source.error().detail.empty() ? source.error().message : source.error().detail;
            facts.containersComplete = false;
            if (ext == "utoc") facts.packageIdsComplete = false;
            facts.containers.push_back(std::move(container));
            continue;
        }
        if (ext == "utoc") {
            if (source.value()->size() > limits.maxTocBytes) {
                container.status = ParseStatus::Unsupported;
                container.detail = "Too large to read.";
                facts.packageIdsComplete = false;
            } else {
                TocParseOptions options;
                options.chunkIdsOnly = true;
                IoStoreToc toc = parseIoStoreToc(*source.value(), options);
                container.status = toc.status;
                container.detail = toc.detail;
                container.version = toc.version;
                container.flags = toc.flags;
                container.entries = toc.entryCount;
                container.compressionMethods = toc.compressionMethods;
                if (toc.status == ParseStatus::Parsed) {
                    facts.maxTocVersion = std::max(facts.maxTocVersion, toc.version);
                    if (toc.signedContainer()) facts.anySignedContainer = true;
                    if (toc.encrypted()) facts.anyEncrypted = true;
                    facts.packageIds.insert(facts.packageIds.end(), toc.packageIds.begin(), toc.packageIds.end());
                    for (const auto& method : toc.compressionMethods) addUnique(facts.compressionMethods, method);
                } else {
                    facts.packageIdsComplete = false;
                }
            }
        } else {
            PakFile pak = parsePak(*source.value(), true);
            container.status = pak.status;
            container.detail = pak.detail;
            container.version = pak.version;
            container.encryptedIndex = pak.encryptedIndex;
            container.compressionMethods = pak.compressionMethods;
            if (pak.status == ParseStatus::Parsed) {
                facts.maxPakVersion = std::max(facts.maxPakVersion, pak.version);
                if (pak.encryptedIndex) facts.anyEncrypted = true;
                for (const auto& method : pak.compressionMethods) addUnique(facts.compressionMethods, method);
            }
        }
        if (container.status != ParseStatus::Parsed) facts.containersComplete = false;
        facts.containers.push_back(std::move(container));
    }
    const bool anyToc = std::any_of(facts.containers.begin(), facts.containers.end(), [](const GameContainer& c) {
        return c.kind == "utoc" && c.status == ParseStatus::Parsed;
    });
    if (!anyToc) facts.packageIdsComplete = false;  // pak-only games list packages by name, not id
    std::sort(facts.packageIds.begin(), facts.packageIds.end());
    facts.packageIds.erase(std::unique(facts.packageIds.begin(), facts.packageIds.end()), facts.packageIds.end());
    if (facts.containers.empty()) {
        facts.reason = "The game's paks folder has no .utoc or .pak files.";
        facts.containersComplete = false;
        facts.packageIdsComplete = false;
    }
    return facts;
}

}  // namespace akeno::unreal
