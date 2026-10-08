// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 5: installing mods without touching game files.
//
// A checked download is unpacked once more and kept in mods/<TITLE_ID>/<download id>/files/.
// Applying builds a fresh overlay from copies of every enabled mod's files (later mods win) in
// Akeno's staging folder, validates it, and moves it into ShadowMountPlus's backport folder for
// the title (<backports root>/<TITLE_ID>), which SMP layers over the game at the next launch.
// The move is a rename on the same filesystem, written to the recovery journal first. Vanilla
// removes Akeno's backport folder. A backport folder Akeno did not create is never touched.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "akeno/core/AppPaths.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/OperationJournal.hpp"
#include "akeno/core/Result.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/mods/Catalog.hpp"
#include "akeno/providers/IModProvider.hpp"
#include "akeno/security/SafeFs.hpp"

namespace akeno::install {

// SMP's default scan path is /data/homebrew; its backports live in <scanpath>/backports.
inline constexpr std::string_view kDefaultBackportsRoot = "/data/homebrew/backports";
// SMP redirects at most this many entries for installed packages.
inline constexpr std::size_t kPackageRedirectLimit = 256;

struct StoredFile {
    std::string installPath;  // relative to the game root
    std::string storePath;    // relative to the mod's files/ folder (the archive path)
    std::uint64_t size = 0;
    std::string sha256;
};

struct InstalledMod {
    std::string downloadId;
    providers::ModRef mod;
    std::string name;
    std::string version;
    bool enabled = true;
    std::uint64_t bytes = 0;
    std::string installedAt;
    std::vector<StoredFile> files;
    bool pcSource = false;  // retained so activation can repeat the no-replacement check
};

// mods/<TITLE_ID>/state.json
struct TitleState {
    static constexpr int kSchemaVersion = 1;
    std::string titleId;
    std::vector<InstalledMod> mods;  // load order: later entries win
    bool overlayActive = false;
    std::string backportPath;
    std::string backportDevice;  // st_dev / st_ino of the folder Akeno moved into place
    std::string backportInode;
    std::string appliedAt;

    std::size_t enabledCount() const;
    const InstalledMod* find(const std::string& downloadId) const;
};

struct InstallEnvironment {
    const security::SafeFs& fs;  // application storage
    AppPaths paths;
    OperationJournal& journal;
    bool interruptedOperationPending = false;
    std::filesystem::path backportsRoot{std::string(kDefaultBackportsRoot)};
    std::uint64_t storageReserve = limits::kStorageSafetyReserveBytes;
    std::function<Result<security::StorageSpace>(const std::filesystem::path&)> storageQuery;  // default statvfs
};

struct InstallRequest {
    std::string downloadId;
    std::filesystem::path archive;  // the verified download
    mods::ArchiveFormat format = mods::ArchiveFormat::Unknown;
    providers::ModRef mod;
    std::string name;
    std::string version;
    std::string titleId;
    std::optional<games::SourceType> sourceType;
    std::string archiveRoot;
    std::string targetPrefix;
    providers::CompatibilityStatus catalogueStatus = providers::CompatibilityStatus::Unknown;
    bool catalogueInstallable = false;
    // A PC mod (Nexus, GameBanana): it may only add files, never replace the game's own, and
    // only where the game's folder can be checked.
    bool pcSource = false;
    std::string gameFolder;  // the game's files (folder games); empty when they cannot be read
};

// The title whose overlay is changed. `mounted` and `installedPkg` come from ShadowMountPlus.
struct TitleTarget {
    std::string titleId;
    bool mounted = false;
    bool installedPkg = false;
    // Where the game is. SMP reads the backports of the game's own scan path, so a folder or
    // image game outside the parent of the backports root is refused. Empty: test titles only.
    std::string installPath;
};

struct ApplyResult {
    std::size_t mods = 0;
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    bool vanilla = false;  // no overlay is active afterwards
    std::filesystem::path backport;
};

std::filesystem::path statePath(const AppPaths& paths, const std::string& titleId);
std::filesystem::path backportPath(const InstallEnvironment& env, const std::string& titleId);

// The stored state, reconciled with the disk: an overlay that is no longer where Akeno put it
// counts as inactive. A missing state is an empty one.
Result<TitleState> loadTitleState(const InstallEnvironment& env, const std::string& titleId);

// Unpacks, analyses and keeps the mod (enabled, last in load order). Refuses anything the
// analysis does not allow to install. Does not change the overlay.
Result<InstalledMod> storeMod(const InstallRequest& request, InstallEnvironment& env);

// Builds, validates and activates the overlay from the enabled mods; with none, removes
// Akeno's overlay (Vanilla).
Result<ApplyResult> applyOverlay(const TitleTarget& target, InstallEnvironment& env);

Status setModEnabled(InstallEnvironment& env, const std::string& titleId, const std::string& downloadId, bool enabled);
// Disables every mod and applies: the game runs unmodified at the next start.
Result<ApplyResult> setVanilla(const TitleTarget& target, InstallEnvironment& env);
// Deletes a stored mod; it must be disabled and the overlay applied without it first.
Status removeStoredMod(InstallEnvironment& env, const std::string& titleId, const std::string& downloadId);

}  // namespace akeno::install
