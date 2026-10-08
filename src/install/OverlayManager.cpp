// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/install/OverlayManager.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <map>
#include <set>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/install/InstallLog.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/mods/ModAnalyzer.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/mods/ModPreparation.hpp"
#include "akeno/security/PathGuard.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::install {

namespace fs = std::filesystem;
using logging::logger;

namespace {

constexpr std::size_t kMaxStateBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxPathBytes = 1023;
constexpr std::size_t kMaxDepth = 64;

bool isSafeId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
    });
}

// Relative, '/'-separated, no empty, "." or ".." components, no backslashes or control bytes.
bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.size() > kMaxPathBytes || path.front() == '/') return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part) {
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
        }
        start = end + 1;
    }
    return true;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Top-level names SMP or the system treat specially: never part of an Akeno overlay.
bool isReservedTopLevel(std::string_view installPath) {
    const std::string first = lower(installPath.substr(0, installPath.find('/')));
    return first == "fakelib" || first == "fakelib2" || first == "sce_sys" || first == "sce_module";
}

struct DirIdentity {
    std::string device;
    std::string inode;
};

std::optional<DirIdentity> identityOf(const fs::path& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return std::nullopt;
    return DirIdentity{std::to_string(info.st_dev), std::to_string(info.st_ino)};
}

bool existsNoFollow(const fs::path& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

// ENOENT below a readable game root means a new path; other lookup failures do not.
Result<bool> gamePathExists(const fs::path& root, const std::string& relative) {
    auto normalized = security::normalizeAbsolute(root.string());
    if (!normalized || !isSafeRelativePath(relative)) {
        return makeError(ErrorCode::SafetyViolation, "The game file mapping is not a safe absolute/relative path pair.");
    }
    struct stat info {};
    if (::lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return makeError(ErrorCode::SafetyViolation, "The game's folder cannot be inspected; PC replacements cannot be ruled out.");
    }
    fs::path current = root;
    const fs::path rel(relative);
    for (auto it = rel.begin(); it != rel.end(); ++it) {
        current /= *it;
        if (::lstat(current.c_str(), &info) != 0) {
            if (errno == ENOENT) return false;
            return makeError(ErrorCode::IoError, "A game path could not be inspected.", relative);
        }
        if (S_ISLNK(info.st_mode) || (std::next(it) != rel.end() && !S_ISDIR(info.st_mode))) {
            return makeError(ErrorCode::SafetyViolation, "A game path contains a link or a non-directory parent.", relative);
        }
    }
    return true;
}

Result<std::string> fileHead(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return makeError(ErrorCode::IoError, "Could not inspect an overlay file.", path.string());
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return makeError(ErrorCode::SafetyViolation, "An overlay entry is not a regular file.", path.string());
    }
    std::string head(archives::kHeadBytes, '\0');
    ssize_t count;
    do { count = ::read(fd, head.data(), head.size()); } while (count < 0 && errno == EINTR);
    ::close(fd);
    if (count < 0) return makeError(ErrorCode::IoError, "Could not read an overlay file.", path.string());
    head.resize(static_cast<std::size_t>(count));
    return head;
}

json::Json toJson(const TitleState& state) {
    json::Json mods = json::Json::array();
    for (const auto& mod : state.mods) {
        json::Json files = json::Json::array();
        for (const auto& file : mod.files) {
            files.push_back({{"installPath", file.installPath},
                             {"storePath", file.storePath},
                             {"size", file.size},
                             {"sha256", file.sha256}});
        }
        json::Json entry = {{"downloadId", mod.downloadId},
                            {"provider", mod.mod.providerId},
                            {"modId", mod.mod.modId},
                            {"name", mod.name},
                            {"version", mod.version},
                            {"enabled", mod.enabled},
                            {"pcSource", mod.pcSource},
                            {"bytes", mod.bytes},
                            {"installedAt", mod.installedAt},
                            {"archiveSha256", mod.archiveSha256},
                            {"files", std::move(files)}};
        if (mod.activationRecorded) {
            entry["activation"] = {{"allowed", mod.activationAllowed},
                                   {"outcome", mod.outcome},
                                   {"mappingConfidence", mod.mappingConfidence},
                                   {"mappingRule", mod.mappingRule},
                                   {"archiveRoot", mod.archiveRoot},
                                   {"targetPrefix", mod.targetPrefix}};
        }
        mods.push_back(std::move(entry));
    }
    return {{"schemaVersion", TitleState::kSchemaVersion},
            {"titleId", state.titleId},
            {"overlayActive", state.overlayActive},
            {"backportPath", state.backportPath},
            {"backportDevice", state.backportDevice},
            {"backportInode", state.backportInode},
            {"appliedAt", state.appliedAt},
            {"mods", std::move(mods)}};
}

Result<TitleState> fromJson(const json::Json& root, const std::string& titleId) {
    auto invalid = [](std::string why) {
        return makeError(ErrorCode::SchemaError, "The list of installed mods is damaged: " + std::move(why));
    };
    if (!root.is_object() || json::getInt(root, "schemaVersion").value_or(0) != TitleState::kSchemaVersion) {
        return invalid("unknown format");
    }
    TitleState state;
    state.titleId = json::getString(root, "titleId").value_or("");
    if (state.titleId != titleId) return invalid("it belongs to another game");
    state.overlayActive = json::getBool(root, "overlayActive").value_or(false);
    state.backportPath = json::getString(root, "backportPath").value_or("");
    state.backportDevice = json::getString(root, "backportDevice").value_or("");
    state.backportInode = json::getString(root, "backportInode").value_or("");
    state.appliedAt = json::getString(root, "appliedAt").value_or("");
    const json::Json* mods = json::getArray(root, "mods");
    if (mods == nullptr) return invalid("no mod list");
    for (const auto& item : *mods) {
        if (!item.is_object()) return invalid("a mod entry is not an object");
        InstalledMod mod;
        mod.downloadId = json::getString(item, "downloadId").value_or("");
        if (!isSafeId(mod.downloadId)) return invalid("a mod has an invalid id");
        mod.mod.providerId = json::getString(item, "provider").value_or("");
        mod.pcSource = json::getBool(item, "pcSource").value_or(false) ||
                       mod.mod.providerId == "nexus" || mod.mod.providerId == "gamebanana";
        mod.mod.modId = json::getString(item, "modId").value_or("");
        mod.name = json::getString(item, "name").value_or(mod.downloadId);
        mod.version = json::getString(item, "version").value_or("");
        mod.enabled = json::getBool(item, "enabled").value_or(false);
        mod.installedAt = json::getString(item, "installedAt").value_or("");
        mod.archiveSha256 = json::getString(item, "archiveSha256").value_or("");
        if (!mod.archiveSha256.empty() && !security::isSha256Hex(mod.archiveSha256)) return invalid("a mod has an invalid archive hash");
        if (const json::Json* activation = json::getObject(item, "activation")) {
            mod.activationRecorded = true;
            mod.activationAllowed = json::getBool(*activation, "allowed").value_or(false);
            mod.outcome = json::displayString(*activation, "outcome", {}, 64);
            mod.mappingConfidence = json::displayString(*activation, "mappingConfidence", {}, 32);
            mod.mappingRule = json::displayString(*activation, "mappingRule", {}, 32);
            mod.archiveRoot = json::displayString(*activation, "archiveRoot", {}, kMaxPathBytes);
            mod.targetPrefix = json::displayString(*activation, "targetPrefix", {}, kMaxPathBytes);
        }
        const json::Json* files = json::getArray(item, "files");
        if (files == nullptr) return invalid("a mod has no file list");
        for (const auto& entry : *files) {
            if (!entry.is_object()) return invalid("a file entry is not an object");
            StoredFile file;
            file.installPath = json::getString(entry, "installPath").value_or("");
            file.storePath = json::getString(entry, "storePath").value_or("");
            file.sha256 = json::getString(entry, "sha256").value_or("");
            const auto size = json::getInt(entry, "size").value_or(-1);
            if (!isSafeRelativePath(file.installPath) || !isSafeRelativePath(file.storePath) ||
                isReservedTopLevel(file.installPath) || file.sha256.size() != 64 || size < 0) {
                return invalid("a file entry is not valid");
            }
            file.size = static_cast<std::uint64_t>(size);
            mod.bytes += file.size;
            mod.files.push_back(std::move(file));
        }
        if (state.find(mod.downloadId) != nullptr) return invalid("a mod is listed twice");
        state.mods.push_back(std::move(mod));
    }
    return state;
}

Status saveState(const InstallEnvironment& env, const TitleState& state) {
    if (state.mods.empty() && !state.overlayActive) {
        // Nothing left for this game: no folder either.
        return env.fs.removeTree(env.paths.mods() / state.titleId);
    }
    AKENO_TRY(env.fs.createDirectories(env.paths.mods() / state.titleId));
    return env.fs.writeFileAtomic(statePath(env.paths, state.titleId), toJson(state).dump(2));
}

Status checkSpace(const InstallEnvironment& env, const fs::path& where, std::uint64_t needed, std::string_view what) {
    auto query = env.storageQuery ? env.storageQuery : security::queryStorageSpace;
    auto space = query(where);
    if (!space) {
        return makeError(ErrorCode::IoError, "Could not check the free space, so nothing was changed.",
                         space.error().describe());
    }
    if (space->availableBytes < needed + env.storageReserve) {
        return makeError(ErrorCode::NoSpace, strings::concat("Not enough free space ", what, ": it needs ",
                                                             strings::formatBytes(needed), " and Akeno always keeps ",
                                                             strings::formatBytes(env.storageReserve), " free."));
    }
    return {};
}

Status checkCommon(const InstallEnvironment& env, const std::string& titleId) {
    if (!games::isValidTitleId(titleId)) {
        return makeError(ErrorCode::InvalidArgument, "The game's title ID is not valid.", titleId);
    }
    if (env.interruptedOperationPending) {
        return makeError(ErrorCode::Busy,
                         "An earlier operation was interrupted. Resolve it in the recovery prompt first.");
    }
    return {};
}

// The backports root must be a real folder on the same filesystem as Akeno's storage, because
// activation is a rename. Creates it (one level, below an existing real folder) when missing.
Status prepareBackportsRoot(const InstallEnvironment& env) {
    const fs::path& root = env.backportsRoot;
    struct stat info {};
    if (::lstat(root.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            return makeError(ErrorCode::SafetyViolation, "The ShadowMountPlus backports location is not a folder.",
                             root.string());
        }
    } else {
        struct stat parent {};
        if (::lstat(root.parent_path().c_str(), &parent) != 0 || !S_ISDIR(parent.st_mode)) {
            return makeError(ErrorCode::NotFound,
                             "The ShadowMountPlus folder for backports does not exist, so mods cannot be applied.",
                             root.parent_path().string());
        }
        auto guard = security::WriteGuard::create({root});
        if (!guard) return std::move(guard).error();
        security::SafeFs rootFs(std::move(guard).value());
        AKENO_TRY(rootFs.createDirectories(root));
    }
    struct stat app {};
    if (::stat(env.paths.staging().c_str(), &app) != 0 || ::stat(root.c_str(), &info) != 0) {
        return makeError(ErrorCode::IoError, "Could not inspect the storage of the backports folder.", root.string());
    }
    if (app.st_dev != info.st_dev) {
        return makeError(ErrorCode::Unsupported,
                         "The backports folder is on a different drive than Akeno's storage, so an overlay cannot "
                         "be switched safely in one step.",
                         root.string());
    }
    return {};
}

// Only regular files and folders, as built; counts entries for the package redirect limit.
Status scanTree(const fs::path& dir, std::size_t& entries, int depth) {
    if (depth > static_cast<int>(kMaxDepth)) {
        return makeError(ErrorCode::SafetyViolation, "The overlay is nested too deeply.", dir.string());
    }
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) return makeError(ErrorCode::IoError, "Could not read the overlay.", dir.string());
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(handle)) {
        std::string_view name = entry->d_name;
        if (name != "." && name != "..") names.emplace_back(name);
    }
    ::closedir(handle);
    for (const auto& name : names) {
        const fs::path child = dir / name;
        struct stat info {};
        if (::lstat(child.c_str(), &info) != 0) {
            return makeError(ErrorCode::IoError, "Could not inspect the overlay.", child.string());
        }
        ++entries;
        if (S_ISDIR(info.st_mode)) {
            AKENO_TRY(scanTree(child, entries, depth + 1));
        } else if (!S_ISREG(info.st_mode)) {
            return makeError(ErrorCode::SafetyViolation, "The overlay contains something that is not a plain file.",
                             child.string());
        }
    }
    return {};
}

}  // namespace

std::size_t TitleState::enabledCount() const {
    return static_cast<std::size_t>(std::count_if(mods.begin(), mods.end(), [](const auto& m) { return m.enabled; }));
}

const InstalledMod* TitleState::find(const std::string& downloadId) const {
    for (const auto& mod : mods) {
        if (mod.downloadId == downloadId) return &mod;
    }
    return nullptr;
}

fs::path statePath(const AppPaths& paths, const std::string& titleId) {
    return paths.mods() / titleId / "state.json";
}

fs::path backportPath(const InstallEnvironment& env, const std::string& titleId) {
    return env.backportsRoot / titleId;
}

Result<TitleState> loadTitleState(const InstallEnvironment& env, const std::string& titleId) {
    if (!games::isValidTitleId(titleId)) {
        return makeError(ErrorCode::InvalidArgument, "The game's title ID is not valid.", titleId);
    }
    const fs::path file = statePath(env.paths, titleId);
    TitleState state;
    state.titleId = titleId;
    if (!existsNoFollow(file)) return state;
    auto text = security::readFileBounded(file, kMaxStateBytes);
    if (!text) return std::move(text).error();
    auto parsed = json::parseBounded(text.value(), kMaxStateBytes);
    if (!parsed) return std::move(parsed).error();
    auto loaded = fromJson(parsed.value(), titleId);
    if (!loaded) return loaded;
    state = std::move(loaded).value();
    if (state.overlayActive) {
        auto identity = identityOf(backportPath(env, titleId));
        if (!identity || identity->device != state.backportDevice || identity->inode != state.backportInode ||
            state.backportPath != backportPath(env, titleId).string()) {
            logger().warn("install", titleId + ": the overlay is no longer where Akeno put it; treated as off");
            state.overlayActive = false;
        }
    }
    return state;
}

namespace {

std::string requestContext(const InstallRequest& request) {
    return strings::concat("title=", request.titleId, " game-version=", request.gameVersion.empty() ? "unknown" : request.gameVersion,
                           " provider=", request.mod.providerId, " mod=", request.mod.modId, " mod-version=", request.version,
                           " download=", request.downloadId,
                           " archive-sha256=", request.archiveSha256.empty() ? "not-recorded" : request.archiveSha256);
}

std::string joinReasons(const std::vector<std::string>& reasons) {
    std::string text;
    for (const auto& reason : reasons) text += text.empty() ? reason : " / " + reason;
    return text;
}

}  // namespace

Result<InstalledMod> storeMod(const InstallRequest& request, InstallEnvironment& env) {
    const std::string context = requestContext(request);
    AKENO_TRY(checkCommon(env, request.titleId));
    if (!isSafeId(request.downloadId)) {
        return makeError(ErrorCode::InvalidArgument, "The download id is not valid.", request.downloadId);
    }
    auto loaded = loadTitleState(env, request.titleId);
    if (!loaded) return std::move(loaded).error();
    TitleState state = std::move(loaded).value();
    if (state.find(request.downloadId) != nullptr) {
        return makeError(ErrorCode::AlreadyExists, "This download is already installed for the game.");
    }
    logStage(Stage::DownloadComplete, StageResult::Ok, context,
             request.archiveSha256.empty() ? "verified when it was downloaded" : "SHA-256 verified when it was downloaded");

    archives::SecureExtractor extractor(env.fs);
    auto listing = extractor.inspect(request.archive, request.format);
    if (!listing) {
        logStage(Stage::ArchiveChecked, StageResult::Failed, context, listing.error().describe());
        return std::move(listing).error();
    }
    AKENO_TRY(checkSpace(env, env.paths.mods(), listing->totalBytes, "to keep this mod"));

    const std::string operationId =
        strings::concat("install-", request.downloadId, "-", strings::utcTimestampCompact());
    const fs::path work = env.paths.staging() / operationId;
    OperationState op;
    op.operationId = operationId;
    op.kind = "install";
    op.titleId = request.titleId;
    op.description = "Installing " + request.name + " " + request.version;
    op.stagingPaths = {work};
    AKENO_TRY(env.journal.begin(op));
    auto fail = [&](Error error) -> Error {
        if (auto removed = env.fs.removeTree(work); !removed) {
            logger().error("install", "could not delete " + work.string() + ": " + removed.error().describe());
            return error;  // the journal stays; the next start offers to clean up
        }
        (void)env.journal.complete();
        return error;
    };

    (void)env.journal.recordStep("extract");
    auto tree = extractor.extract(request.archive, request.format, work / "files");
    if (!tree) {
        logStage(Stage::ArchiveChecked, StageResult::Failed, context, tree.error().describe());
        return fail(std::move(tree).error());
    }

    // The same preparation the check showed: layout, content, containers, compatibility.
    const bool curated = request.curated || mods::isCuratedProvider(request.mod.providerId);
    const bool pcSource = request.pcSource || mods::isPcProvider(request.mod.providerId);
    mods::PreparationRequest prep;
    prep.files = tree->files;
    prep.extractedRoot = work / "files";
    prep.game = mods::GameContext{request.titleId, request.gameVersion, request.contentId, request.sourceType,
                                  request.installedPkg, request.gameFolder};
    prep.provider = request.mod.providerId;
    prep.modId = request.mod.modId;
    prep.modVersion = request.version;
    prep.curated = curated && !pcSource;
    prep.pcSource = pcSource;
    if (prep.curated) prep.manifestArchiveRoot = request.archiveRoot;
    prep.manifestTargetPrefix = request.targetPrefix;
    prep.catalogueStatus = request.catalogueStatus;
    prep.catalogueInstallable = request.catalogueInstallable;
    prep.registry = env.registry;
    for (const auto& other : state.mods) {
        mods::InstalledModPaths paths{other.downloadId, other.name, {}};
        for (const auto& file : other.files) paths.installPaths.push_back(file.installPath);
        prep.installedMods.push_back(std::move(paths));
    }
    const mods::Preparation prepared = mods::prepareMod(prep);
    const mods::ModAnalysis& analysis = prepared.analysis;
    const compatibility::Assessment& assessment = prepared.assessment;
    const std::string decision = strings::concat(
        "outcome ", compatibility::toString(assessment.outcome), ", category ", compatibility::toString(assessment.category),
        ", mapping ", mods::toString(assessment.mappingConfidence), " (rule ", prepared.layout.rule, ", archive root '",
        prepared.layout.archiveRoot, "' -> '", prepared.layout.targetPrefix, "'), loading ",
        compatibility::toString(assessment.loading), ", platform ", compatibility::toString(assessment.platform));
    if (!assessment.activationAllowed || analysis.hasBlockers()) {
        std::string why = assessment.blockedReasons.empty() ? "its compatibility is " + std::string(compatibility::toString(assessment.outcome))
                                                            : assessment.blockedReasons.front();
        for (const auto& finding : analysis.findings) {
            if (assessment.blockedReasons.empty() && finding.level == mods::FindingLevel::Blocker) {
                why = finding.message;
                break;
            }
        }
        logStage(Stage::ArchiveChecked, StageResult::Refused, context,
                 decision + "; rejected because: " + joinReasons(assessment.blockedReasons.empty() ? std::vector<std::string>{why}
                                                                                                    : assessment.blockedReasons));
        return fail(makeError(ErrorCode::SafetyViolation, "This mod cannot be installed: " + why));
    }
    logStage(Stage::ArchiveChecked, StageResult::Ok, context,
             strings::concat(decision, "; ", analysis.installCount, " files to install"));

    if (pcSource) {
        // A PC replacement has no established PS5 data-format compatibility.
        if (request.gameFolder.empty()) {
            return fail(makeError(ErrorCode::Unsupported,
                                  "This is a PC mod, and Akeno cannot see this game's files to make sure it replaces "
                                  "none of them, so it is not installed."));
        }
        std::size_t replaced = 0;
        std::string example;
        for (const auto& file : analysis.files) {
            if (file.installPath.empty()) continue;
            auto exists = gamePathExists(request.gameFolder, file.installPath);
            if (!exists) return fail(exists.error());
            if (exists.value()) {
                if (replaced++ == 0) example = file.installPath;
            }
        }
        if (replaced > 0) {
            logStage(Stage::ArchiveChecked, StageResult::Refused, context,
                     strings::concat("would replace ", replaced, " game file(s), e.g. ", example));
            return fail(makeError(ErrorCode::SafetyViolation,
                                  strings::concat("This PC mod would replace ", replaced,
                                                  " of the game's own files with PC versions, which can crash the "
                                                  "game or the console. Akeno does not install it."),
                                  example));
        }
    }

    InstalledMod mod;
    mod.downloadId = request.downloadId;
    mod.mod = request.mod;
    mod.name = request.name;
    mod.version = request.version;
    mod.pcSource = pcSource;
    mod.enabled = true;
    mod.installedAt = strings::utcTimestamp();
    mod.archiveSha256 = security::isSha256Hex(request.archiveSha256) ? request.archiveSha256 : std::string();
    mod.activationRecorded = true;
    mod.activationAllowed = assessment.activationAllowed;
    mod.outcome = std::string(compatibility::toString(assessment.outcome));
    mod.mappingConfidence = std::string(mods::toString(assessment.mappingConfidence));
    mod.mappingRule = prepared.layout.rule;
    mod.archiveRoot = prepared.layout.archiveRoot;
    mod.targetPrefix = prepared.layout.targetPrefix;
    for (const auto& file : analysis.files) {
        if (file.installPath.empty()) continue;
        if (!isSafeRelativePath(file.installPath) || isReservedTopLevel(file.installPath)) {
            return fail(makeError(ErrorCode::SafetyViolation, "This mod would install to a protected place.",
                                  file.installPath));
        }
        mod.files.push_back({file.installPath, file.archivePath, file.size, file.sha256});
        mod.bytes += file.size;
    }

    (void)env.journal.recordStep("store");
    const fs::path destination = env.paths.mods() / request.titleId / request.downloadId;
    if (auto made = env.fs.createDirectories(destination.parent_path()); !made) return fail(made.error());
    if (existsNoFollow(destination)) {
        // Left by an earlier attempt that never reached the state file.
        if (auto removed = env.fs.removeTree(destination); !removed) return fail(removed.error());
    }
    if (auto moved = env.fs.rename(work, destination); !moved) {
        logStage(Stage::FilesInstalled, StageResult::Failed, context, moved.error().describe());
        return fail(moved.error());
    }
    state.mods.push_back(mod);
    if (auto saved = saveState(env, state); !saved) {
        (void)env.fs.removeTree(destination);
        (void)env.journal.complete();
        logStage(Stage::FilesInstalled, StageResult::RolledBack, context,
                 "the list of installed mods could not be saved; the stored copy was removed: " + saved.error().describe());
        return saved.error();
    }
    (void)env.journal.complete();
    logStage(Stage::FilesInstalled, StageResult::Ok, context,
             strings::concat(mod.files.size(), " files, ", strings::formatBytes(mod.bytes), ", SHA-256 of every file recorded, in ",
                             destination.string()));
    for (std::size_t i = 0; i < mod.files.size() && i < 50; ++i) {
        logger().info("install", "  mapped " + mod.files[i].storePath + " -> " + mod.files[i].installPath);
    }
    return mod;
}

Result<ApplyResult> applyOverlay(const TitleTarget& target, InstallEnvironment& env) {
    const std::string& titleId = target.titleId;
    AKENO_TRY(checkCommon(env, titleId));
    if (target.mounted) {
        return makeError(ErrorCode::Busy, "The game is running or mounted. Close it, then try again.");
    }
    const std::string scanPath = env.backportsRoot.parent_path().string() + "/";
    if (!target.installedPkg && !target.installPath.empty() && !strings::startsWith(target.installPath, scanPath)) {
        return makeError(ErrorCode::Unsupported,
                         "Only games in " + scanPath + " and installed packages are supported so far. For a game on "
                         "another drive, ShadowMountPlus would look for mods in a different place.",
                         target.installPath);
    }
    auto loaded = loadTitleState(env, titleId);
    if (!loaded) return std::move(loaded).error();
    TitleState state = std::move(loaded).value();

    const fs::path backport = backportPath(env, titleId);
    const bool backportExists = existsNoFollow(backport);
    if (backportExists && !state.overlayActive) {
        return makeError(ErrorCode::SafetyViolation,
                         "This game already has a backport folder that Akeno did not create (for example a firmware "
                         "backport). Akeno never changes it, so mods cannot be applied to this game.",
                         backport.string());
    }

    std::vector<const InstalledMod*> enabled;
    std::uint64_t bytes = 0;
    for (const auto& mod : state.mods) {
        if (!mod.enabled) continue;
        // A PC mod is activated only with a recorded, positive compatibility decision. Mods stored
        // by earlier versions (whose archive layout was copied unmapped) can only be turned off.
        if ((mod.pcSource && !mod.activationRecorded) || (mod.activationRecorded && !mod.activationAllowed)) {
            const std::string where = mod.files.empty() ? std::string() : " (for example " + mod.files.front().installPath + ")";
            logStage(Stage::OverlayPublished, StageResult::Refused, "title=" + titleId + " download=" + mod.downloadId,
                     "no recorded activation decision for a PC mod" + where);
            return makeError(ErrorCode::SafetyViolation,
                             mod.name + " was installed without a verified installation path" + where +
                                 ". Akeno does not activate it again: turn it off or remove it in Installed Mods.",
                             mod.downloadId);
        }
        enabled.push_back(&mod);
        bytes += mod.bytes;
    }
    const std::string context = strings::concat("title=", titleId, " mods=", enabled.size(), " backport=", backportPath(env, titleId).string());
    ApplyResult result;
    result.backport = backport;
    result.mods = enabled.size();
    result.vanilla = enabled.empty();
    if (enabled.empty() && !backportExists) {
        if (state.overlayActive) {
            state.overlayActive = false;
            AKENO_TRY(saveState(env, state));
        }
        return result;  // already vanilla
    }

    AKENO_TRY(prepareBackportsRoot(env));
    auto guard = security::WriteGuard::create({env.paths.root, backport});
    if (!guard) return std::move(guard).error();
    const security::SafeFs titleFs(std::move(guard).value());

    const std::string operationId = strings::concat("apply-", titleId, "-", strings::utcTimestampCompact());
    const fs::path work = env.paths.staging() / operationId;
    const fs::path next = work / "next";
    const fs::path previous = work / "previous";
    OperationState op;
    op.operationId = operationId;
    op.kind = "activate";
    op.titleId = titleId;
    op.description = enabled.empty() ? "Switching " + titleId + " to Vanilla" : "Applying mods to " + titleId;
    op.stagingPaths = {work};
    AKENO_TRY(env.journal.begin(op));
    auto fail = [&](Error error) -> Error {
        logStage(Stage::OverlayPublished, StageResult::Failed, context,
                 error.describe() + "; the published overlay was not changed");
        if (auto removed = env.fs.removeTree(work); !removed) {
            logger().error("install", "could not delete " + work.string() + ": " + removed.error().describe());
            return error;
        }
        (void)env.journal.complete();
        return error;
    };

    if (!enabled.empty()) {
        if (auto recorded = env.journal.recordStep("build"); !recorded) return fail(recorded.error());
        if (auto space = checkSpace(env, env.paths.staging(), bytes, "to build the overlay"); !space) {
            return fail(space.error());
        }
        if (auto made = env.fs.createDirectories(next); !made) return fail(made.error());
        std::set<std::string> written;
        std::map<std::string, std::string> spellings;
        for (const InstalledMod* mod : enabled) {
            const fs::path store = env.paths.mods() / titleId / mod->downloadId / "files";
            for (const StoredFile& file : mod->files) {
                const fs::path source = store / file.storePath;
                if (auto safe = env.fs.guard().checkWritable(source); !safe) return fail(safe.error());
                auto head = fileHead(source);
                if (!head) return fail(head.error());
                mods::AnalysisInput input;
                input.files.push_back({file.storePath, file.size, file.sha256, head.value()});
                const auto analysis = mods::analyzeMod(input);
                for (const auto& finding : analysis.findings) {
                    if (finding.level == mods::FindingLevel::Blocker) {
                        return fail(makeError(ErrorCode::SafetyViolation,
                                              "A stored mod no longer passes compatibility checks: " + finding.message,
                                              file.storePath));
                    }
                }
                if (mod->pcSource) {
                    auto exists = gamePathExists(target.installPath, file.installPath);
                    if (!exists) return fail(exists.error());
                    if (exists.value()) return fail(makeError(ErrorCode::SafetyViolation,
                        "A stored PC mod would replace a game file; activation was refused.", file.installPath));
                }
                // Check directory spellings too: Data/a + data/b is also ambiguous.
                fs::path prefix;
                for (const auto& part : fs::path(file.installPath)) {
                    prefix /= part;
                    auto [it, inserted] = spellings.emplace(lower(prefix.string()), prefix.string());
                    if (!inserted && it->second != prefix.string()) {
                        return fail(makeError(ErrorCode::SafetyViolation,
                                              "Overlay paths differ only in upper/lower case.", prefix.string()));
                    }
                }
                auto hash = security::sha256File(source);
                if (!hash || hash.value() != file.sha256) {
                    return fail(makeError(ErrorCode::SchemaError,
                                          "A stored file of " + mod->name + " is missing or changed. Remove the mod "
                                          "and install it again.",
                                          file.storePath));
                }
                if (file.installPath.size() + backport.string().size() + 1 > kMaxPathBytes) {
                    return fail(makeError(ErrorCode::SafetyViolation, "A file path of the overlay is too long.",
                                          file.installPath));
                }
                const fs::path destination = next / file.installPath;
                if (auto made = env.fs.createDirectories(destination.parent_path()); !made) return fail(made.error());
                if (written.count(lower(file.installPath)) != 0) {
                    if (auto removed = env.fs.removeFile(destination); !removed) return fail(removed.error());
                }
                if (auto copied = env.fs.copyFile(source, destination); !copied) return fail(copied.error());
                auto copiedHash = security::sha256File(destination);
                if (!copiedHash || copiedHash.value() != file.sha256) {
                    return fail(makeError(ErrorCode::SchemaError, "An overlay copy failed verification.", file.installPath));
                }
                written.insert(lower(file.installPath));
                ++result.files;
                result.bytes += file.size;
            }
        }
        if (auto recorded = env.journal.recordStep("validate"); !recorded) return fail(recorded.error());
        std::size_t entries = 0;
        if (auto scanned = scanTree(next, entries, 0); !scanned) return fail(scanned.error());
        if (target.installedPkg && entries > kPackageRedirectLimit) {
            return fail(makeError(ErrorCode::Unsupported,
                                  strings::concat("For an installed package ShadowMountPlus can redirect at most ",
                                                  kPackageRedirectLimit, " redirects. This overlay has ", entries,
                                                  " files and folders (a conservative upper bound, not an exact "
                                                  "redirect count); it cannot be approved without the game tree.")));
        }
    }

    // From here the published overlay changes. Each rename is atomic; a crash between them
    // leaves no Akeno backport at this location. Other scan roots/mounted layers are unknown.
    if (auto made = env.fs.createDirectories(work); !made) return fail(made.error());
    if (auto recorded = env.journal.markOverlayTouched(); !recorded) return fail(recorded.error());
    if (auto recorded = env.journal.recordStep("swap"); !recorded) return fail(recorded.error());
    const TitleState originalState = state;
    // Never remove `previous` if restoring the old overlay or its ownership state fails.
    auto restore = [&](Error error) -> Error {
        if (backportExists) {
            if (auto restored = titleFs.rename(previous, backport); !restored) {
                logger().error("install", "previous overlay retained for recovery: " + restored.error().describe());
                logStage(Stage::OverlayPublished, StageResult::Failed, context,
                         error.describe() + "; restoring the previous overlay failed, it is kept in " + previous.string() +
                             " and the recovery journal is kept");
                return error;
            }
        }
        if (auto saved = saveState(env, originalState); !saved) {
            logger().error("install", "overlay ownership needs recovery: " + saved.error().describe());
            logStage(Stage::OverlayPublished, StageResult::Failed, context,
                     error.describe() + "; the previous overlay is back but its ownership record could not be restored");
            return error;
        }
        logStage(Stage::OverlayPublished, StageResult::RolledBack, context,
                 error.describe() + "; the previous overlay and its record were restored");
        if (auto removed = env.fs.removeTree(work); !removed) {
            logger().error("install", "could not delete " + work.string() + ": " + removed.error().describe());
            return error;
        }
        (void)env.journal.complete();
        return error;
    };
    if (backportExists) {
        if (auto moved = titleFs.rename(backport, previous); !moved) return fail(moved.error());
    }
    if (!enabled.empty()) {
        // Record the new overlay first: a rename keeps the folder's identity, so after a crash the
        // state either matches the folder in place or finds none (Vanilla), never a stranger.
        auto identity = identityOf(next);
        if (!identity) return restore(makeError(ErrorCode::IoError, "The new overlay cannot be found.", next.string()));
        state.overlayActive = true;
        state.backportPath = backport.string();
        state.backportDevice = identity->device;
        state.backportInode = identity->inode;
        state.appliedAt = strings::utcTimestamp();
        if (auto saved = saveState(env, state); !saved) {
            return restore(saved.error());
        }
        if (auto moved = titleFs.rename(next, backport); !moved) {
            return restore(moved.error());
        }
    } else {
        state.overlayActive = false;
        state.backportPath.clear();
        state.backportDevice.clear();
        state.backportInode.clear();
        state.appliedAt = strings::utcTimestamp();
        if (auto saved = saveState(env, state); !saved) {
            // Harmless: the stored identity no longer matches anything, so it reads as off.
            logger().warn("install", "Vanilla is active but the mod list could not be saved: " +
                                         saved.error().describe());
        }
    }
    if (auto removed = env.fs.removeTree(work); !removed) {
        logger().warn("install", "could not delete " + work.string() + ": " + removed.error().describe());
    } else {
        (void)env.journal.complete();
    }
    logger().info("install", enabled.empty()
                                 ? titleId + ": Vanilla, Akeno's overlay was removed"
                                 : strings::concat(titleId, ": overlay applied, ", result.mods, " mods, ", result.files,
                                                   " files, ", strings::formatBytes(result.bytes)));
    if (enabled.empty()) {
        logStage(Stage::OverlayPublished, StageResult::Ok, context, "Vanilla: Akeno's overlay folder was removed");
    } else {
        logStage(Stage::OverlayPublished, StageResult::Ok, context,
                 strings::concat(result.files, " files, ", strings::formatBytes(result.bytes),
                                 ", every copy verified by SHA-256; ShadowMountPlus applies it at the next game start"));
        logUnobservedStages(context);
    }
    return result;
}

Status setModEnabled(InstallEnvironment& env, const std::string& titleId, const std::string& downloadId, bool enabled) {
    auto loaded = loadTitleState(env, titleId);
    if (!loaded) return std::move(loaded).error();
    TitleState state = std::move(loaded).value();
    for (auto& mod : state.mods) {
        if (mod.downloadId == downloadId) {
            mod.enabled = enabled;
            return saveState(env, state);
        }
    }
    return makeError(ErrorCode::NotFound, "This mod is not installed for the game.", downloadId);
}

Result<ApplyResult> setVanilla(const TitleTarget& target, InstallEnvironment& env) {
    AKENO_TRY(checkCommon(env, target.titleId));
    if (target.mounted) return makeError(ErrorCode::Busy, "The game is running or mounted. Close it, then try again.");
    auto loaded = loadTitleState(env, target.titleId);
    if (!loaded) return std::move(loaded).error();
    TitleState state = std::move(loaded).value();
    const TitleState originalState = state;
    for (auto& mod : state.mods) mod.enabled = false;
    if (!state.mods.empty()) AKENO_TRY(saveState(env, state));
    auto applied = applyOverlay(target, env);
    if (!applied && !state.mods.empty()) {
        if (auto saved = saveState(env, originalState); !saved) {
            logger().error("install", "could not restore mod selection: " + saved.error().describe());
        }
    }
    return applied;
}

Status removeStoredMod(InstallEnvironment& env, const std::string& titleId, const std::string& downloadId) {
    AKENO_TRY(checkCommon(env, titleId));
    auto loaded = loadTitleState(env, titleId);
    if (!loaded) return std::move(loaded).error();
    TitleState state = std::move(loaded).value();
    auto it = std::find_if(state.mods.begin(), state.mods.end(),
                           [&](const InstalledMod& mod) { return mod.downloadId == downloadId; });
    if (it == state.mods.end()) return makeError(ErrorCode::NotFound, "This mod is not installed for the game.");
    if (it->enabled && state.overlayActive) {
        return makeError(ErrorCode::Busy, "Turn the mod off and apply the change before removing it.");
    }
    AKENO_TRY(env.fs.removeTree(env.paths.mods() / titleId / downloadId));
    state.mods.erase(it);
    return saveState(env, state);
}

}  // namespace akeno::install
