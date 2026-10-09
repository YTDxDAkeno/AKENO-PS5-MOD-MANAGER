// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/diagnostics/DiagnosticExport.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "akeno/archives/SecureExtractor.hpp"
#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/games/ParamJson.hpp"
#include "akeno/logging/Redactor.hpp"
#include "akeno/mods/ModAnalyzer.hpp"
#include "akeno/security/PathGuard.hpp"
#include "akeno/security/SafeOpen.hpp"
#include "akeno/security/Sha256.hpp"
#include "akeno/shadowmount/ShadowMountGameProvider.hpp"
#include "akeno/unreal/UnrealFormats.hpp"

namespace akeno::diagnostics {
namespace fs = std::filesystem;
using json::Json;
using security::describeErrno;
using Clock = std::chrono::steady_clock;

std::string_view toString(Mode mode) noexcept { return mode == Mode::Deep ? "deep" : "quick"; }

Limits quickLimits() {
    Limits limits;
    limits.entries = 50000;
    limits.hashBytes = 8ull * 1024 * 1024 * 1024;
    limits.duration = std::chrono::seconds{180};
    limits.gameDuration = std::chrono::seconds{60};
    limits.gameHashFileBytes = 1024 * 1024;
    return limits;
}

Limits deepLimits() {
    Limits limits;
    limits.entries = 200000;
    limits.hashBytes = 64ull * 1024 * 1024 * 1024;
    limits.duration = std::chrono::seconds{900};
    limits.gameDuration = std::chrono::seconds{840};
    limits.gameHashFileBytes = ~0ull;
    return limits;
}

namespace {

// Directory streams get their own descriptor: closedir() closes it, the walk keeps its own.
DIR* directoryStream(int fd) {
    const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return nullptr;
    DIR* stream = ::fdopendir(copy);
    if (!stream) {
        const int error = errno;
        ::close(copy);
        errno = error;
    }
    return stream;
}

// What a change of the data alters. ctime is not part of it: ShadowMountPlus chmods every entry
// below backports/ ("[BKP] permissions fixed" in its debug.log), which changes only ctime and
// was reported as "changed during inventory" before.
bool sameContent(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}

bool sameMetadata(const struct stat& a, const struct stat& b) {
    return sameContent(a, b) && a.st_mode == b.st_mode && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

Json unavailable(std::string reason) {
    return {{"status", "unavailable"}, {"complete", false}, {"reason", std::move(reason)}};
}

struct Budget {
    Limits limits;
    std::size_t entries = 0;
    std::uint64_t hashed = 0;
    Clock::time_point started = Clock::now();
    Clock::time_point deadline = started + limits.duration;
    const CancellationToken* cancel = nullptr;
    mutable bool timeLimitHit = false;

    Budget(const Limits& l, const CancellationToken* c) : limits(l), started(Clock::now()), deadline(started + l.duration), cancel(c) {}
    bool cancelled() const { return cancel != nullptr && cancel->cancelled(); }
    bool stopped() const {
        if (cancelled()) return true;
        if (Clock::now() >= deadline) {
            timeLimitHit = true;
            return true;
        }
        return false;
    }
    bool entriesExhausted() const { return entries >= limits.entries; }
    std::string reason() const {
        if (cancelled()) return "cancellation";
        if (timeLimitHit) return "time limit";
        return "entry limit";
    }
};

Json readText(const fs::path& path, std::size_t limit, bool tail) {
    security::UniqueFd fd = security::openNoFollow(path, false);
    if (!fd.valid()) return unavailable(describeErrno(errno));
    struct stat before {};
    if (::fstat(fd.get(), &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
        return unavailable("Not a regular readable file");
    const auto size = static_cast<std::uint64_t>(before.st_size);
    if (size > limit && !tail) return unavailable("File exceeds read limit");
    const auto offset = tail && size > limit ? size - limit : 0;
    if (::lseek(fd.get(), static_cast<off_t>(offset), SEEK_SET) < 0) return unavailable("Seek failed: " + describeErrno(errno));
    std::string text(static_cast<std::size_t>(std::min<std::uint64_t>(size, limit)), '\0');
    std::size_t got = 0;
    while (got < text.size()) {
        const auto n = ::read(fd.get(), text.data() + got, text.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    const bool fullRead = got == text.size();
    text.resize(got);
    // Do not export a partial first line that could omit the name of a secret key.
    if (offset > 0) { auto end = text.find('\n'); text = end == std::string::npos ? "" : text.substr(end + 1); }
    struct stat after {};
    // A log that grows while it is read is not damaged evidence: compare content identity only.
    const bool stable = ::fstat(fd.get(), &after) == 0 && sameContent(before, after) && fullRead;
    return {{"status", stable ? "observed" : "changed-during-read"}, {"complete", stable && offset == 0},
            {"truncated", offset != 0}, {"originalBytes", size}, {"text", text}};
}

struct ScanOptions {
    bool analyze = true;  // classify files and run the content checks (mods, overlays)
    bool game = false;    // a game folder: hashing limited by gameHashFileBytes, container headers
};

// The container header of a game .utoc/.pak, read from the already opened file.
Json containerHeader(int fd, const std::string& name) {
    const std::string lower = strings::toLowerAscii(name);
    const bool toc = strings::endsWith(lower, ".utoc");
    if (!toc && !strings::endsWith(lower, ".pak")) return nullptr;
    const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    auto source = unreal::FileSource::fromDescriptor(copy, name);
    if (!source) return {{"status", "unreadable"}};
    if (toc) {
        unreal::TocParseOptions options;
        options.headerOnly = true;
        const auto parsed = unreal::parseIoStoreToc(*source.value(), options);
        Json out{{"format", "IoStore .utoc"}, {"status", std::string(unreal::toString(parsed.status))}};
        if (!parsed.detail.empty()) out["detail"] = parsed.detail;
        if (parsed.version > 0) {
            out["version"] = parsed.version;
            out["chunks"] = parsed.entryCount;
            out["compressed"] = parsed.compressed();
            out["encrypted"] = parsed.encrypted();
            out["signed"] = parsed.signedContainer();
            out["indexed"] = parsed.indexed();
        }
        return out;
    }
    const auto pak = unreal::parsePak(*source.value(), true);
    Json out{{"format", ".pak"}, {"status", std::string(unreal::toString(pak.status))}};
    if (pak.version > 0) {
        out["version"] = pak.version;
        out["encryptedIndex"] = pak.encryptedIndex;
        out["compressionMethods"] = pak.compressionMethods;
    }
    if (pak.status != unreal::ParseStatus::Parsed && !pak.detail.empty()) out["detail"] = pak.detail;
    return out;
}

Json scan(const fs::path& root, Budget& budget, const ScanOptions& options) {
    Json report = {{"root", root.string()}, {"status", "observed"}, {"complete", true}, {"hashesComplete", true},
                   {"entries", Json::array()}, {"findings", Json::array()}, {"snapshotAtomic", false}};
    std::set<std::string> incompleteReasons;
    std::vector<std::string> metadataChanges;
    std::size_t skippedHashes = 0;
    auto issue = [&](std::string path, std::string why, std::string reason = "filesystem") {
        report["complete"] = false;
        incompleteReasons.insert(std::move(reason));
        report["findings"].push_back({{"path", std::move(path)}, {"message", std::move(why)}});
    };
    // Budget stops are reported as such, never as filesystem failures.
    auto stopped = [&](const std::string& path) {
        const std::string reason = budget.reason();
        issue(path, "Not inventoried: the export's " + reason + " was reached", reason);
    };
    security::UniqueFd rootFd = security::openNoFollow(root, true);
    if (!rootFd.valid()) return unavailable(describeErrno(errno));
    struct stat rootInfo {};
    if (::fstat(rootFd.get(), &rootInfo) != 0) return unavailable("Cannot stat the folder: " + describeErrno(errno));
    std::map<std::string, std::string> spellings;
    std::function<void(int, const std::string&, unsigned)> walk;
    walk = [&](int directory, const std::string& prefix, unsigned depth) {
        if (budget.stopped() || budget.entriesExhausted()) { stopped(prefix); return; }
        if (depth > 64) { issue(prefix, "Folders nested deeper than 64 levels were not inventoried", "depth"); return; }
        struct stat before {};
        if (::fstat(directory, &before) != 0) { issue(prefix, "Cannot stat directory: " + describeErrno(errno)); return; }
        DIR* dir = directoryStream(directory);
        if (!dir) { issue(prefix, "Cannot enumerate directory: " + describeErrno(errno)); return; }
        std::vector<std::string> names;
        int readError = 0;
        bool stoppedWhileReading = false;
        for (;;) {
            errno = 0;
            auto* entry = ::readdir(dir);
            if (!entry) { readError = errno; break; }
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (names.size() + budget.entries >= budget.limits.entries || budget.stopped()) {
                stoppedWhileReading = true;
                break;
            }
            names.push_back(std::move(name));
        }
        ::closedir(dir);
        if (readError != 0) issue(prefix, "Directory enumeration failed: " + describeErrno(readError));
        if (stoppedWhileReading) stopped(prefix);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            if (budget.stopped() || budget.entriesExhausted()) { stopped(prefix); break; }
            ++budget.entries;
            const std::string path = prefix + name;
            struct stat info {};
            if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                // ENOENT here usually means the entry was removed after it was listed.
                issue(path, "Cannot stat entry: " + describeErrno(errno));
                continue;
            }
            const bool regular = S_ISREG(info.st_mode), folder = S_ISDIR(info.st_mode);
            Json item = {{"path", path}, {"type", regular ? "file" : folder ? "directory" : "unsupported"}};
            auto validPath = archives::normalizeEntryPath(path, { .maxDepth = 65 });
            if (!validPath) issue(path, "Unsafe or unsupported relative path", "path");
            if ((root / path).string().size() >= 1024) issue(path, "Absolute source path exceeds SMP path limit", "path");
            auto [spelling, inserted] = spellings.emplace(strings::toLowerAscii(path), path);
            if (!inserted && spelling->second != path) issue(path, "Case ambiguity with " + spelling->second, "path");
            if (folder) {
                if (info.st_dev != rootInfo.st_dev) { issue(path, "Nested filesystem not traversed", "nested-filesystem"); }
                else {
                    security::UniqueFd child(::openat(directory, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                    struct stat opened {};
                    if (!child.valid()) issue(path, "Cannot open directory: " + describeErrno(errno));
                    else if (::fstat(child.get(), &opened) != 0) issue(path, "Cannot stat opened directory: " + describeErrno(errno));
                    else if (!sameContent(info, opened)) issue(path, "Directory was replaced between listing and opening");
                    else walk(child.get(), path + "/", depth + 1);
                }
            } else if (regular) {
                item["size"] = info.st_size;
                item["sha256"] = nullptr;
                item["hashStatus"] = "unavailable";
                security::UniqueFd file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
                struct stat opened {};
                if (!file.valid()) {
                    issue(path, "Cannot open file: " + describeErrno(errno));
                } else if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode) || !sameContent(info, opened)) {
                    issue(path, "File was replaced between listing and opening");
                } else {
                    std::array<char, 65536> buffer {};
                    std::string head;
                    security::Sha256 hash;
                    std::uint64_t bytes = 0;
                    int readFailure = 0;
                    const auto size = static_cast<std::uint64_t>(std::max<off_t>(0, info.st_size));
                    const bool sizeAllowed = !options.game || size <= budget.limits.gameHashFileBytes;
                    const bool budgetAllowed = size <= budget.limits.hashBytes - std::min(budget.hashed, budget.limits.hashBytes);
                    const bool hashAllowed = sizeAllowed && budgetAllowed;
                    bool interrupted = false;
                    for (;;) {
                        if (budget.stopped()) { interrupted = true; break; }
                        const auto capacity = hashAllowed ? buffer.size() : archives::kHeadBytes;
                        const auto n = ::read(file.get(), buffer.data(), capacity);
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0) { readFailure = errno; break; }
                        if (n == 0) break;
                        const auto amount = static_cast<std::size_t>(n);
                        if (head.empty()) head.assign(buffer.data(), std::min(amount, archives::kHeadBytes));
                        if (!hashAllowed) break;
                        hash.update(buffer.data(), amount); bytes += amount; budget.hashed += amount;
                    }
                    struct stat after {};
                    const bool unchanged = ::fstat(file.get(), &after) == 0 && sameContent(info, after);
                    if (hashAllowed && !interrupted && readFailure == 0 && unchanged && bytes == size) {
                        item["sha256"] = hash.finishHex(); item["hashStatus"] = "complete";
                    } else if (!sizeAllowed) {
                        item["hashStatus"] = "skipped-quick-mode";
                        ++skippedHashes;
                    } else if (!budgetAllowed) {
                        item["hashStatus"] = "byte-budget";
                    } else if (interrupted) {
                        item["hashStatus"] = "interrupted";
                        issue(path, "Hash not finished: the export's " + budget.reason() + " was reached", budget.reason());
                    } else if (readFailure != 0) {
                        item["hashStatus"] = "read-failed";
                        issue(path, "Read failed while hashing: " + describeErrno(readFailure));
                    } else {
                        item["hashStatus"] = "changed-during-read";
                        issue(path, "File contents changed while it was hashed");
                    }
                    if (options.game && budget.limits.gameContainerHeaders && !budget.stopped()) {
                        Json header = containerHeader(file.get(), name);
                        if (!header.is_null()) item["container"] = std::move(header);
                    }
                    if (options.analyze) {
                        mods::AnalysisInput input;
                        input.files.push_back({path, size, {}, head});
                        auto analysis = mods::analyzeMod(input);
                        item["kind"] = std::string(mods::toString(mods::classifyFile(path, head)));
                        item["compatibility"] = "unknown";
                        item["findings"] = Json::array();
                        for (const auto& finding : analysis.findings) {
                            item["findings"].push_back({{"level", mods::toString(finding.level)}, {"message", finding.message}});
                            if (finding.level == mods::FindingLevel::Blocker) item["compatibility"] = "blocked";
                        }
                        if (strings::toLowerAscii(name) == "modconfig.json") {
                            auto config = readText(root / path, 256 * 1024, false);
                            item["loaderDependencies"] = Json::array();
                            item["dependencyParseStatus"] = "unavailable-or-invalid";
                            if (config.contains("text") && config["status"] == "observed") {
                                auto parsed = json::parseBounded(config["text"].get<std::string>(), 256 * 1024);
                                if (parsed && parsed->is_object()) {
                                    item["dependencyParseStatus"] = "observed";
                                    if (const auto* dependencies = json::getArray(*parsed, "ModDependencies")) {
                                        for (const auto& dependency : *dependencies) {
                                            if (item["loaderDependencies"].size() >= 128) { item["dependencyParseStatus"] = "truncated"; break; }
                                            if (dependency.is_string()) item["loaderDependencies"].push_back(
                                                logging::redactSecrets(strings::sanitizeForDisplay(dependency.get<std::string>(), 256)));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (item["sha256"].is_null()) report["hashesComplete"] = false;
            } else { issue(path, "Link or special file skipped without reading", "links"); }
            report["entries"].push_back(std::move(item));
        }
        struct stat after {};
        if (::fstat(directory, &after) != 0) issue(prefix, "Cannot stat directory after listing: " + describeErrno(errno));
        else if (!sameContent(before, after)) issue(prefix, "Directory contents changed during inventory (entries added, removed or renamed)");
        else if (!sameMetadata(before, after) && metadataChanges.size() < 100) metadataChanges.push_back(prefix.empty() ? "." : prefix);
    };
    walk(rootFd.get(), "", 0);
    // Children are visited before their directory entry; normalize report order.
    std::sort(report["entries"].begin(), report["entries"].end(), [](const Json& a, const Json& b) {
        return a["path"].get<std::string>() < b["path"].get<std::string>();
    });
    if (report["complete"] == false) {
        report["status"] = "partial";
        report["incompleteReasons"] = Json(std::vector<std::string>(incompleteReasons.begin(), incompleteReasons.end()));
    }
    if (skippedHashes > 0) {
        report["hashesSkipped"] = skippedHashes;
        report["hashesSkippedReason"] = strings::concat("Quick diagnostics: game files larger than ",
            strings::formatBytes(budget.limits.gameHashFileBytes), " are listed with size and type but not hashed");
    }
    if (!metadataChanges.empty()) {
        report["metadataChanges"] = {{"paths", metadataChanges},
            {"note", "Permissions or ownership changed while reading (ctime); contents did not. ShadowMountPlus "
                     "repairs permissions below backports/ during its scans."}};
    }
    return report;
}

std::string directoryStatus(const fs::path& path) {
    security::UniqueFd fd = security::openNoFollow(path, true);
    if (fd.valid()) return "directory";
    if (errno == ENOENT) return "absent";
    // ENOTDIR may be a symlink rejected in an ancestor: not proof of absence.
    return "unknown";
}
bool under(std::string_view root, std::string_view path) {
    return path == root || (path.size() > root.size() && path.substr(0, root.size()) == root &&
                           (root == "/" || path[root.size()] == '/'));
}
std::vector<std::string> defaultRoots() {
    std::vector<std::string> roots{"/data/homebrew", "/data/etaHEN/games", "/mnt/ext0/homebrew",
        "/mnt/ext0/etaHEN/games", "/mnt/ext1/homebrew", "/mnt/ext1/etaHEN/games"};
    for (const auto* suffix : {"/homebrew", "/etaHEN/games", ""})
        for (int i = 0; i < 8; ++i) roots.push_back("/mnt/usb" + std::to_string(i) + suffix);
    roots.push_back("/mnt/ext0"); roots.push_back("/mnt/ext1");
    return roots;
}
bool safeId(const std::string& id) {
    return !id.empty() && id.size() <= 64 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
} // namespace

Json inventory(const fs::path& root, const Limits& limits, const CancellationToken* cancel, bool analyze) {
    Budget budget(limits, cancel);
    return scan(root, budget, {analyze, false});
}

Json overlaySelection(const games::GameInfo& game, const shadowmount::VersionInfo& version,
                      const Json& settings, const fs::path& akenoBackport) {
    Json result = {{"status", "unknown"}, {"selectedPath", nullptr}, {"candidates", Json::array()},
                   {"mountObserved", false}, {"gameConsumptionObserved", false},
                   {"basis", "Released upstream 1.7beta4 rules; read-only prediction at collection time, not a mount guarantee"}};
    auto unknown = [&](const std::string& why) { result["reason"] = why; return result; };
    if (!games::isValidTitleId(game.titleId) || version.apiVersion != 1 || version.shadowMountVersion != "1.7beta4")
        return unknown("Unknown title or unsupported resolver version");
    if (!settings.is_object() || !settings.contains("scan_paths") || !settings["scan_paths"].is_array() ||
        !settings.contains("scan_path_count") || !settings["scan_path_count"].is_number_integer() ||
        settings["scan_path_count"].get<std::int64_t>() != static_cast<std::int64_t>(settings["scan_paths"].size()) ||
        settings["scan_paths"].size() > 128) return unknown("Incomplete or malformed custom scan-path settings");
    std::vector<std::string> roots;
    for (const auto& p : settings["scan_paths"]) {
        if (!p.is_string()) return unknown("Invalid scan root");
        const auto value = p.get<std::string>();
        auto normalized = security::normalizeAbsolute(value);
        if (!normalized || normalized->string() != value) return unknown("Noncanonical scan root; precedence cannot be inferred");
        if (std::find(roots.begin(), roots.end(), value) == roots.end()) roots.push_back(value);
    }
    result["usesReleaseDefaults"] = roots.empty();
    if (roots.empty()) roots = defaultRoots();
    roots.push_back("/mnt/shadowmnt/pfsc"); roots.push_back("/mnt/shadowmnt");
    std::string owner;
    if (!game.installedPkg) {
        auto source = security::normalizeAbsolute(game.installPath);
        if (!source || source->string() != game.installPath || under("/mnt/shadowmnt", game.installPath))
            return unknown("Physical source/owning scan root is not established");
        for (const auto& root : roots) if (under(root, game.installPath) && root.size() > owner.size()) owner = root;
        if (owner.empty()) return unknown("No owning scan root; cached/manual source resolution is unavailable");
    }
    result["inferredOwningRoot"] = owner;
    std::vector<std::string> order;
    if (!owner.empty()) order.push_back(owner);
    order.insert(order.end(), roots.begin(), roots.end());
    order.push_back("/data/homebrew"); // explicit upstream fallback, even with custom scanpaths
    std::set<std::string> seen;
    for (const auto& root : order) {
        if (under("/mnt/shadowmnt", root) || !seen.insert(root).second) continue;
        const auto candidate = fs::path(root) / "backports" / game.titleId;
        if (candidate.string().size() >= 1024) return unknown("Candidate exceeds upstream path limit");
        const auto status = directoryStatus(candidate);
        result["candidates"].push_back({{"path", candidate.string()}, {"status", status}});
        if (status == "unknown") return unknown("A higher-priority candidate cannot be inspected without following links");
        if (status == "directory") {
            result["selectedPath"] = candidate.string();
            result["status"] = candidate == akenoBackport ? "predicted-akeno" : "predicted-other";
            return result;
        }
    }
    result["status"] = "none-observed";
    return result;
}

Result<fs::path> exportReport(const Request& request, const security::SafeFs& safeFs,
    shadowmount::ShadowMountClient& client, const CancellationToken* cancel,
    const std::function<void(std::string)>& progress) {
    if (!request.titleId.empty() && !games::isValidTitleId(request.titleId))
        return makeError(ErrorCode::InvalidArgument, "Invalid diagnostic title ID");
    Budget budget(request.limits, cancel);
    Json phases = Json::object();
    Json report = {{"schemaVersion", 3}, {"generatedAt", strings::utcTimestamp()},
        {"mode", "PS5-native-read-only"}, {"diagnosticsMode", std::string(toString(request.mode))},
        {"activationPerformed", false}, {"gameLaunchPerformed", false},
        {"hardwareVerified", false}, {"gameAssetsExported", false},
        {"firmware", {{"known", request.firmware.known}, {"raw", request.firmware.raw}, {"display", request.firmware.display}}},
        {"akeno", {{"version", build::version()}, {"revision", build::gitRevision()}, {"sourceSha256", build::sourceFingerprint()},
                   {"target", build::target()}, {"compiler", build::compiler()}}},
        {"titles", Json::array()}, {"logs", Json::array()}};
    if (progress) progress("Reading ShadowMountPlus information");
    auto phaseStart = Clock::now();
    auto version = client.version();
    auto settings = client.diagnosticSettings();
    auto liveGames = client.games();
    phases["shadowMountApi"] = secondsSince(phaseStart);
    std::vector<games::GameInfo> games = request.cachedGames;
    report["gameListSource"] = "cached; selection unknown";
    if (version && version->apiVersion == 1 && liveGames && liveGames->skipped.empty()) {
        games.clear();
        for (const auto& game : liveGames->games) games.push_back(shadowmount::toGameInfo(game));
        report["gameListSource"] = "live-api";
    }
    report["shadowMountPlus"] = {{"version", version ? Json(version->shadowMountVersion) : Json(nullptr)},
        {"apiVersion", version ? Json(version->apiVersion) : Json(nullptr)},
        {"settings", settings ? settings.value() : unavailable(settings.error().message)},
        {"unexposedSettings", "mount_read_only, persistent_image_mounts, image rules and effective cached owning root are not exposed by beta4 settings API"}};
    if (!liveGames) report["gameListError"] = liveGames.error().message;
    std::set<std::string> titles;
    if (!request.titleId.empty()) titles.insert(request.titleId);
    else {
        security::UniqueFd mods = security::openNoFollow(request.paths.mods(), true);
        DIR* dir = mods.valid() ? directoryStream(mods.get()) : nullptr;
        if (dir) {
            for (;;) {
                errno = 0;  // per entry: anything in the loop body may set errno
                auto* entry = ::readdir(dir);
                if (!entry) {
                    if (errno != 0) report["titleDiscoveryError"] = "Mod directory enumeration failed: " + describeErrno(errno);
                    break;
                }
                if (games::isValidTitleId(entry->d_name)) titles.insert(entry->d_name);
                if (titles.size() > 32) break;
            }
            ::closedir(dir);
        } else report["titleDiscoveryError"] = "Installed mod directory cannot be enumerated: " + describeErrno(errno);
    }
    report["titleLimitReached"] = titles.size() > 32;
    std::size_t count = 0;
    std::size_t mappings = 0;
    bool gameTimeLimit = false;
    for (const auto& title : titles) {
        if (count++ >= 32 || budget.cancelled()) break;
        if (progress) progress("Inventorying " + title + " (files are only read)");
        games::GameInfo game;
        game.titleId = title;
        bool found = false;
        for (const auto& candidate : games) if (candidate.titleId == title) { game = candidate; found = true; break; }
        bool physicalTitleMatches = false;
        if (found) {
            // Read only the physical folder's metadata. Do not use an overlaid runtime tree as vanilla.
            auto metadata = game.sourceType == games::SourceType::Folder
                ? readText(fs::path(game.installPath) / "sce_sys/param.json", games::kMaxParamJsonBytes, false)
                : unavailable("Physical folder unavailable");
            if (metadata.contains("text")) {
                auto param = json::parseBounded(metadata["text"].get<std::string>(), games::kMaxParamJsonBytes);
                physicalTitleMatches = metadata["status"] == "observed" && param &&
                    json::getString(*param, "titleId") == title;
                auto v = games::contentVersionFromParamJson(metadata["text"].get<std::string>());
                if (v && game.version.empty() && physicalTitleMatches) { game.version = *v; game.versionSource = "physical sce_sys/param.json"; }
            }
        }
        Json item = {{"titleId", title}, {"gameVersion", game.version.empty() ? Json(nullptr) : Json(game.version)},
            {"versionSource", game.versionSource}, {"sourceType", games::toString(game.sourceType)},
            {"installPath", game.installPath}, {"mountedFlag", game.mounted},
            {"mountedFlagProvesStopped", false}, {"installedMods", Json::array()}};
        const auto backport = request.backportsRoot / title;
        // 1. Selection first: it needs no file contents, so no inventory budget can starve it.
        const bool selectionInputs = version && settings && found && report["gameListSource"] == "live-api";
        item["selection"] = selectionInputs ? overlaySelection(game, *version, *settings, backport)
                                            : Json{{"status", "unknown"}, {"reason", "Fresh compatible game/version/settings evidence unavailable"}};
        item["selection"]["evaluatedBeforeInventory"] = true;
        auto beforeSelection = item["selection"];
        // 2. Akeno's own trees: the recorded state, stored mods and the published overlay.
        phaseStart = Clock::now();
        std::set<std::string> recordedFiles, recordedFolders;
        bool stateParsed = false;
        const auto statePath = request.paths.mods() / title / "state.json";
        auto stateText = readText(statePath, 16 * 1024 * 1024, false);
        if (stateText.contains("text") && stateText["status"] == "observed") {
            auto parsed = json::parseBounded(stateText["text"].get<std::string>(), 16 * 1024 * 1024);
            if (parsed && parsed->is_object() && json::getString(*parsed, "titleId") == title &&
                json::getInt(*parsed, "schemaVersion") == 1 && json::getArray(*parsed, "mods")) {
                stateParsed = true;
                item["recordedOverlayActive"] = json::getBool(*parsed, "overlayActive").value_or(false);
                item["recordedOwnership"] = Json::object();
                for (const auto* key : {"backportPath", "backportDevice", "backportInode", "appliedAt"})
                    item["recordedOwnership"][key] = json::getString(*parsed, key).value_or("");
                auto* mods = json::getArray(*parsed, "mods");
                std::size_t modCount = 0;
                for (const auto& mod : *mods) {
                    if (budget.cancelled() || modCount++ >= 256) { item["installedModsPartial"] = true; break; }
                    const auto id = json::getString(mod, "downloadId").value_or("");
                    if (!safeId(id)) { item["stateError"] = "Invalid mod ID"; continue; }
                    Json stored = {{"downloadId", id}, {"provider", json::getString(mod, "provider").value_or("")},
                        {"enabled", json::getBool(mod, "enabled").value_or(false)},
                        {"pcSourceRecorded", json::getBool(mod, "pcSource").value_or(false)}, {"mapping", Json::array()}};
                    if (const auto* activation = json::getObject(mod, "activation")) {
                        stored["recordedActivation"] = Json::object();
                        for (const auto* key : {"outcome", "mappingConfidence", "mappingRule", "archiveRoot", "targetPrefix"})
                            stored["recordedActivation"][key] = json::getString(*activation, key).value_or("");
                        stored["recordedActivation"]["allowed"] = json::getBool(*activation, "allowed").value_or(false);
                    } else {
                        stored["recordedActivation"] = nullptr;  // installed before decisions were recorded
                    }
                    if (const auto* files = json::getArray(mod, "files")) {
                        std::size_t mappingCount = 0;
                        for (const auto& file : *files) {
                            if (mappingCount++ >= 20000 || mappings++ >= request.limits.entries) { stored["mappingPartial"] = true; break; }
                            Json mapping = Json::object();
                            for (const auto* key : {"installPath", "storePath", "size", "sha256"})
                                if (file.is_object() && file.contains(key) && (file[key].is_string() || file[key].is_number_integer())) mapping[key] = file[key];
                            const std::string installPath = json::getString(file, "installPath").value_or("");
                            mapping["safePaths"] = archives::normalizeEntryPath(installPath).ok() &&
                                archives::normalizeEntryPath(json::getString(file, "storePath").value_or("")).ok();
                            if (!installPath.empty()) {
                                recordedFiles.insert(strings::toLowerAscii(installPath));
                                std::string prefix;
                                for (const auto& part : strings::split(installPath, '/')) {
                                    if (!prefix.empty()) recordedFolders.insert(strings::toLowerAscii(prefix));
                                    prefix = prefix.empty() ? part : prefix + "/" + part;
                                }
                            }
                            stored["mapping"].push_back(std::move(mapping));
                        }
                    }
                    stored["inventory"] = scan(request.paths.mods() / title / id / "files", budget, {true, false});
                    if (stored["inventory"].contains("entries")) {
                        std::map<std::string, Json> actual;
                        for (const auto& entry : stored["inventory"]["entries"]) actual.emplace(entry["path"].get<std::string>(), entry);
                        for (auto& mapping : stored["mapping"]) {
                            const auto path = json::getString(mapping, "storePath").value_or("");
                            const auto entry = actual.find(path);
                            mapping["integrity"] = "unknown";
                            if (entry != actual.end() && entry->second.value("hashStatus", "") == "complete") {
                                mapping["observedSha256"] = entry->second["sha256"];
                                mapping["integrity"] = mapping.value("sha256", Json()) == entry->second["sha256"] &&
                                    mapping.value("size", Json()) == entry->second["size"] ? "matches-record" : "mismatch";
                            }
                        }
                    }
                    item["installedMods"].push_back(std::move(stored));
                }
            } else item["stateError"] = "Invalid state schema; no recovery or mutation attempted";
        } else item["stateError"] = stateText.value("reason", "State unavailable or changed");
        item["overlay"] = scan(backport, budget, {true, false});
        if (stateParsed && item["overlay"].contains("entries")) {
            // Entries Akeno did not record: a read-write unionfs layer creates "shadow" folders in
            // its upper layer (the backport) for game folders it looks up, and the game itself
            // may write through it. Both are hypotheses until a mounted run is compared.
            std::size_t unrecorded = 0;
            auto& entries = item["overlay"]["entries"];
            for (auto& entry : entries) {
                const auto path = entry["path"].get<std::string>();
                const auto lower = strings::toLowerAscii(path);
                const bool folder = entry["type"] == "directory";
                const bool recorded = folder ? recordedFolders.count(lower) != 0 : recordedFiles.count(lower) != 0;
                entry["recordedByAkeno"] = recorded;
                if (recorded) continue;
                ++unrecorded;
                const bool empty = folder && std::none_of(entries.begin(), entries.end(), [&](const Json& other) {
                    return strings::startsWith(other["path"].get<std::string>(), path + "/");
                });
                if (empty) entry["note"] = "Empty folder Akeno did not create; possibly a unionfs shadow folder from a mounted run";
            }
            item["overlayUnrecordedEntries"] = unrecorded;
        }
        phases[title + ".akenoTrees"] = secondsSince(phaseStart);
        // 3. The game's physical folder, in its own time slice: never the runtime mount.
        phaseStart = Clock::now();
        item["vanilla"] = unavailable("Only physical folder games can be inventoried without mounting; runtime paths are never treated as vanilla");
        if (found && physicalTitleMatches && game.sourceType == games::SourceType::Folder && !game.installedPkg &&
            !game.installPath.empty() && !under("/system_ex", game.installPath) && !under("/mnt/shadowmnt", game.installPath) &&
            !under("/mnt/sandbox", game.installPath) && !under(request.backportsRoot.string(), game.installPath)) {
            const auto overall = budget.deadline;
            budget.deadline = std::min(overall, Clock::now() + request.limits.gameDuration);
            const bool hitBefore = budget.timeLimitHit;
            budget.timeLimitHit = false;
            item["vanilla"] = scan(game.installPath, budget, {false, true});
            item["vanilla"]["basis"] = "Physical source folder reported by SMP; not an independently authenticated retail baseline";
            item["vanilla"]["gameTimeLimitSeconds"] = request.limits.gameDuration.count();
            if (budget.timeLimitHit) gameTimeLimit = true;
            budget.timeLimitHit = hitBefore || (budget.timeLimitHit && Clock::now() >= overall);
            budget.deadline = overall;
        }
        phases[title + ".gameFolder"] = secondsSince(phaseStart);
        item["physicalTitleMetadataMatches"] = physicalTitleMatches;
        if (item["overlay"].contains("entries")) {
            std::map<std::string, Json> baseline;
            if (item["vanilla"].contains("entries")) for (const auto& entry : item["vanilla"]["entries"])
                baseline.emplace(entry["path"].get<std::string>(), entry);
            for (auto& entry : item["overlay"]["entries"]) {
                const auto path = entry["path"].get<std::string>();
                const auto original = baseline.find(path);
                entry["gamePathCheck"] = "unknown";
                if (original != baseline.end()) {
                    entry["gamePathCheck"] = entry["type"] == original->second["type"] ? "existing-same-type" : "type-conflict";
                    if (entry.value("hashStatus", "") == "complete" && original->second.value("hashStatus", "") == "complete")
                        entry["sameHashAsSource"] = entry["sha256"] == original->second["sha256"];
                } else if (item["vanilla"].value("complete", false)) entry["gamePathCheck"] = "absent-from-source-inventory";
            }
        }
        // 4. The filesystem candidates once more: a change during the inventory voids the prediction.
        if (selectionInputs) {
            auto after = overlaySelection(game, *version, *settings, backport);
            after["evaluatedBeforeInventory"] = true;
            if (after != beforeSelection) item["selection"] = {{"status", "unknown"}, {"reason", "Candidates changed during inventory"}, {"before", beforeSelection}, {"after", after}};
        }
        report["titles"].push_back(std::move(item));
    }
    // The API evidence is checked again even when a time limit was reached: it is cheap, and a
    // prediction must not survive a configuration change. Only cancellation skips it.
    if (!budget.cancelled()) {
        if (progress) progress("Checking ShadowMountPlus information again");
        auto afterSettings = client.diagnosticSettings();
        auto afterVersion = client.version();
        auto afterGames = client.games();
        bool changed = !settings || !afterSettings || *settings != *afterSettings || !version || !afterVersion ||
            version->apiVersion != afterVersion->apiVersion || version->shadowMountVersion != afterVersion->shadowMountVersion ||
            !liveGames || !afterGames || !afterGames->skipped.empty();
        if (!changed) {
            for (const auto& before : liveGames->games) {
                auto match = std::find_if(afterGames->games.begin(), afterGames->games.end(), [&](const auto& g) { return g.titleId == before.titleId; });
                if (match == afterGames->games.end() || match->path != before.path || match->sourceType != before.sourceType ||
                    match->installedPkg != before.installedPkg || match->version != before.version) changed = true;
            }
        }
        for (auto& title : report["titles"]) {
            if (changed) title["selection"] = {{"status", "unknown"}, {"reason", "API evidence changed or could not be rechecked"}};
            else title["selection"]["apiEvidenceRechecked"] = true;
        }
    } else for (auto& title : report["titles"]) title["selection"] = {{"status", "unknown"}, {"reason", "Export cancelled; final evidence not rechecked"}};
    for (const auto& log : request.logFiles) {
        auto data = readText(log, 256 * 1024, true);
        if (data.contains("text")) data["text"] = logging::redactSecrets(data["text"].get<std::string>());
        data["path"] = log.string(); report["logs"].push_back(std::move(data));
    }
    report["failureLogLimitations"] = "Only available allowlisted Akeno/SMP logs; no kernel panic capture is asserted";
    report["shadowMountPlus"]["onDiskConfiguration"] = unavailable("No readable on-disk config supplied");
    if (!request.smpConfigFile.empty()) {
        auto config = readText(request.smpConfigFile, 256 * 1024, false);
        if (config.contains("text") && config["status"] == "observed") {
            Json fields = Json::array();
            const std::set<std::string> allowed{"scanpath", "scan_depth", "read_only", "mount_read_only",
                "persistent_image_mounts", "backport_fakelib", "global_fakelib", "global_fakelib_priority",
                "global_fakelib_enabled", "global_fakelib_game_priority", "global_fakelib_path",
                "fakelib_exclude", "global_fakelib_exclude", "image_ro", "image_rw",
                "kstuff_game_auto_toggle", "kstuff_crash_detection", "stability_wait_s"};
            for (const auto& line : strings::split(config["text"].get<std::string>(), '\n')) {
                const auto equal = line.find('=');
                if (equal == std::string::npos) continue;
                auto trim = [](std::string value) {
                    const auto first = value.find_first_not_of(" \t\r");
                    if (first == std::string::npos) return std::string();
                    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
                };
                const auto key = strings::toLowerAscii(trim(line.substr(0, equal)));
                if (!allowed.count(key) || fields.size() >= 256) continue;
                const auto value = trim(line.substr(equal + 1));
                fields.push_back({{"key", key}, {"value", logging::redactSecrets(strings::sanitizeForDisplay(value, 1024))}});
            }
            report["shadowMountPlus"]["onDiskConfiguration"] = {{"status", "observed"}, {"fields", fields},
                {"path", request.smpConfigFile.string()}, {"isRuntimeEvidence", false}};
        }
    }
    auto journal = readText(request.paths.operationJournal(), 256 * 1024, false);
    report["operationJournal"] = unavailable("Absent, unreadable or invalid; no recovery attempted");
    if (journal.contains("text") && journal["status"] == "observed") {
        auto parsed = json::parseBounded(journal["text"].get<std::string>(), 256 * 1024);
        if (parsed && parsed->is_object()) {
            Json summary = {{"status", "observed"}};
            for (const auto* key : {"operationId", "kind", "titleId", "step", "startedAt", "updatedAt"})
                summary[key] = logging::redactSecrets(json::getString(*parsed, key).value_or(""));
            summary["activeOverlayTouched"] = json::getBool(*parsed, "activeOverlayTouched").value_or(true);
            report["operationJournal"] = std::move(summary);
        }
    }
    report["cancelled"] = budget.cancelled();
    report["timeLimitReached"] = budget.timeLimitHit || Clock::now() >= budget.deadline;
    report["gameInventoryTimeLimitReached"] = gameTimeLimit;
    report["entriesInspected"] = budget.entries;
    report["bytesHashed"] = budget.hashed;
    report["phaseSeconds"] = std::move(phases);
    report["limits"] = {{"entries", request.limits.entries}, {"hashBytes", request.limits.hashBytes},
                        {"seconds", request.limits.duration.count()}, {"gameSeconds", request.limits.gameDuration.count()},
                        {"gameHashFileBytes", request.limits.gameHashFileBytes}};
    report["partialResultsPossible"] = true;
    const std::string encoded = report.dump(2, ' ', false, Json::error_handler_t::replace);
    if (encoded.size() > 64 * 1024 * 1024) return makeError(ErrorCode::ResponseTooLarge, "Diagnostic report exceeds 64 MiB; export one title at a time");
    const auto outputDir = request.paths.logs() / "diagnostics";
    AKENO_TRY(safeFs.createDirectories(outputDir));
    // Unique name, never replace a prior report within the same second.
    const auto suffix = Clock::now().time_since_epoch().count();
    const auto output = outputDir / strings::concat("diagnostic-", request.titleId.empty() ? "installed" : request.titleId,
                                                   "-", toString(request.mode), "-", strings::utcTimestampCompact(), "-", suffix, ".json");
    AKENO_TRY(safeFs.writeFileAtomic(output, encoded));
    return output;
}
} // namespace akeno::diagnostics
