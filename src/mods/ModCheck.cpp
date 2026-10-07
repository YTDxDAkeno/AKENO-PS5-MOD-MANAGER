// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/mods/ModCheck.hpp"

#include <system_error>

#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::mods {

namespace fs = std::filesystem;
using logging::logger;
using providers::CompatibilityStatus;

namespace {

constexpr std::size_t kMaxReportBytes = 16 * 1024 * 1024;

std::optional<FileKind> parseFileKind(std::string_view text) {
    for (FileKind kind : {FileKind::Asset, FileKind::Text, FileKind::Image, FileKind::Config, FileKind::Script,
                          FileKind::Archive, FileKind::WindowsCode, FileKind::NativeCode, FileKind::Junk}) {
        if (toString(kind) == text) return kind;
    }
    return std::nullopt;
}

std::optional<FindingLevel> parseFindingLevel(std::string_view text) {
    for (FindingLevel level : {FindingLevel::Info, FindingLevel::Warning, FindingLevel::Blocker}) {
        if (toString(level) == text) return level;
    }
    return std::nullopt;
}

bool isValidDownloadId(std::string_view id) {
    if (id.size() != 16) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

Error invalidReport(std::string what) {
    return makeError(ErrorCode::SchemaError, "The stored check result is not valid.", std::move(what));
}

}  // namespace

std::string_view toString(CompatibilityStatus status) noexcept {
    switch (status) {
        case CompatibilityStatus::Verified: return "verified";
        case CompatibilityStatus::Likely: return "likely";
        case CompatibilityStatus::Experimental: return "experimental";
        case CompatibilityStatus::Unknown: return "unknown";
        case CompatibilityStatus::PcOnly: return "pc-only";
        case CompatibilityStatus::Incompatible: return "incompatible";
    }
    return "unknown";
}

std::optional<CompatibilityStatus> parseCompatibilityStatus(std::string_view text) noexcept {
    for (CompatibilityStatus status : {CompatibilityStatus::Verified, CompatibilityStatus::Likely,
                                       CompatibilityStatus::Experimental, CompatibilityStatus::Unknown,
                                       CompatibilityStatus::PcOnly, CompatibilityStatus::Incompatible}) {
        if (toString(status) == text) return status;
    }
    return std::nullopt;
}

std::string_view describe(CheckPhase phase) noexcept {
    switch (phase) {
        case CheckPhase::Inspecting: return "Reading the archive";
        case CheckPhase::Extracting: return "Unpacking into the staging folder";
        case CheckPhase::Analysing: return "Looking at every file";
        case CheckPhase::CleaningUp: return "Deleting the staging folder";
    }
    return "";
}

Result<ModCheckReport> runModCheck(const ModCheckRequest& request, ModCheckEnvironment& env,
                                   const CancellationToken* cancel,
                                   const std::function<void(CheckPhase, double)>& progress) {
    auto report = [&](CheckPhase phase, double fraction) {
        if (progress) progress(phase, fraction);
    };
    if (env.interruptedOperationPending) {
        return makeError(ErrorCode::Busy,
                         "An interrupted operation must be resolved first (Home > Resolve the interrupted operation).");
    }
    if (!isValidDownloadId(request.downloadId)) {
        return makeError(ErrorCode::InvalidArgument, "Invalid download.", request.downloadId);
    }

    archives::SecureExtractor extractor(env.fs, env.limits);
    report(CheckPhase::Inspecting, 0.0);
    auto listing = extractor.inspect(request.archive, request.format, cancel);
    if (!listing) return std::move(listing).error();

    // Unpacking needs room for every file plus the reserve that always stays free.
    auto query = env.storageQuery ? env.storageQuery : security::queryStorageSpace;
    auto space = query(env.paths.staging());
    if (!space) {
        return makeError(ErrorCode::IoError, "Could not check the free space, so nothing was unpacked.",
                         space.error().describe());
    }
    if (space->availableBytes < listing->totalBytes + env.storageReserve) {
        return makeError(ErrorCode::NoSpace,
                         strings::concat("Not enough free space to unpack this mod: it needs ",
                                         strings::formatBytes(listing->totalBytes), " and Akeno always keeps ",
                                         strings::formatBytes(env.storageReserve), " free."));
    }

    const std::string operationId = strings::concat("check-", request.downloadId, "-", strings::utcTimestampCompact());
    const fs::path staging = env.paths.staging() / operationId;
    OperationState state;
    state.operationId = operationId;
    state.kind = "check";
    state.titleId = request.titleId;
    state.description = "Checking " + request.displayName + " " + request.modVersion;
    state.stagingPaths = {staging};
    AKENO_TRY(env.journal.begin(state));

    auto finish = [&](Result<ModCheckReport> result) -> Result<ModCheckReport> {
        report(CheckPhase::CleaningUp, 0.0);
        std::error_code ec;
        if (fs::exists(fs::symlink_status(staging, ec))) {
            auto removed = env.fs.removeTree(staging);
            if (!removed) {
                // Keep the journal: the next start offers to clean up.
                logger().error("check", "could not delete the staging folder: " + removed.error().describe());
                return result ? Result<ModCheckReport>(removed.error()) : result;
            }
        }
        auto completed = env.journal.complete();
        if (!completed) logger().warn("check", "could not clear the journal: " + completed.error().describe());
        return result;
    };

    (void)env.journal.recordStep("extract");
    auto tree = extractor.extract(request.archive, request.format, staging, cancel,
                                  [&](std::uint64_t done, std::uint64_t total) {
                                      report(CheckPhase::Extracting,
                                             total == 0 ? 1.0 : static_cast<double>(done) / static_cast<double>(total));
                                  });
    if (!tree) return finish(std::move(tree).error());

    (void)env.journal.recordStep("analyse");
    report(CheckPhase::Analysing, 0.0);
    AnalysisInput input;
    input.files = tree->files;
    input.archiveRoot = request.archiveRoot;
    input.targetPrefix = request.targetPrefix;
    input.titleId = request.titleId;
    input.sourceType = request.sourceType;
    input.catalogueStatus = request.catalogueStatus;
    input.catalogueInstallable = request.catalogueInstallable;

    ModCheckReport result;
    result.downloadId = request.downloadId;
    result.mod = request.mod;
    result.displayName = request.displayName;
    result.modVersion = request.modVersion;
    result.titleId = request.titleId;
    result.checkedAt = strings::utcTimestamp();
    result.archiveFormat = tree->listing.formatName;
    result.archiveFiles = tree->listing.fileCount;
    result.unpackedBytes = tree->listing.totalBytes;
    result.analysis = analyzeMod(input);
    logger().info("check", strings::concat(request.downloadId, ": ", result.analysis.installCount, " files to install, ",
                                           result.analysis.findings.size(), " findings, label ",
                                           toString(result.analysis.status)));
    return finish(std::move(result));
}

fs::path reportPath(const AppPaths& paths, const std::string& downloadId) {
    return paths.cache() / "analysis" / (downloadId + ".json");
}

Status saveReport(const security::SafeFs& fs, const AppPaths& paths, const ModCheckReport& report) {
    json::Json document = json::Json::object();
    document["schemaVersion"] = ModCheckReport::kSchemaVersion;
    document["downloadId"] = report.downloadId;
    document["mod"] = {{"provider", report.mod.providerId}, {"id", report.mod.modId}};
    document["name"] = report.displayName;
    document["version"] = report.modVersion;
    document["titleId"] = report.titleId;
    document["checkedAt"] = report.checkedAt;
    document["archive"] = {{"format", report.archiveFormat},
                           {"files", report.archiveFiles},
                           {"unpackedBytes", report.unpackedBytes}};
    const ModAnalysis& a = report.analysis;
    document["status"] = std::string(toString(a.status));
    document["installable"] = a.installable;
    document["engineHint"] = a.engineHint;
    json::Json findings = json::Json::array();
    for (const auto& finding : a.findings) {
        findings.push_back({{"level", std::string(toString(finding.level))},
                            {"message", finding.message},
                            {"path", finding.path}});
    }
    document["findings"] = std::move(findings);
    json::Json files = json::Json::array();
    for (const auto& file : a.files) {
        files.push_back({{"archivePath", file.archivePath},
                         {"installPath", file.installPath},
                         {"size", file.size},
                         {"sha256", file.sha256},
                         {"kind", std::string(toString(file.kind))}});
    }
    document["files"] = std::move(files);
    AKENO_TRY(fs.createDirectories(paths.cache() / "analysis"));
    return fs.writeFileAtomic(reportPath(paths, report.downloadId),
                              document.dump(1, ' ', false, json::Json::error_handler_t::replace));
}

Result<ModCheckReport> loadReport(const AppPaths& paths, const std::string& downloadId) {
    if (!isValidDownloadId(downloadId)) return invalidReport("bad id");
    auto text = security::readFileBounded(reportPath(paths, downloadId), kMaxReportBytes);
    if (!text) return std::move(text).error();
    auto parsed = json::parseBounded(text.value(), kMaxReportBytes);
    if (!parsed) return std::move(parsed).error();
    const json::Json& d = parsed.value();
    if (!d.is_object() || json::getInt(d, "schemaVersion") != ModCheckReport::kSchemaVersion ||
        json::getString(d, "downloadId") != downloadId) {
        return invalidReport("schema or id");
    }
    ModCheckReport r;
    r.downloadId = downloadId;
    if (const json::Json* mod = json::getObject(d, "mod")) {
        r.mod.providerId = json::displayString(*mod, "provider");
        r.mod.modId = json::displayString(*mod, "id");
    }
    r.displayName = json::displayString(d, "name");
    r.modVersion = json::displayString(d, "version");
    r.titleId = json::displayString(d, "titleId");
    r.checkedAt = json::displayString(d, "checkedAt");
    if (const json::Json* archive = json::getObject(d, "archive")) {
        r.archiveFormat = json::displayString(*archive, "format");
        r.archiveFiles = static_cast<std::size_t>(std::max<std::int64_t>(0, json::getInt(*archive, "files").value_or(0)));
        r.unpackedBytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, json::getInt(*archive, "unpackedBytes").value_or(0)));
    }
    ModAnalysis& a = r.analysis;
    auto status = parseCompatibilityStatus(json::getString(d, "status").value_or(""));
    if (!status) return invalidReport("status");
    a.status = *status;
    a.installable = json::getBool(d, "installable").value_or(false);
    a.engineHint = json::displayString(d, "engineHint");
    const json::Json* findings = json::getArray(d, "findings");
    const json::Json* files = json::getArray(d, "files");
    if (findings == nullptr || files == nullptr || findings->size() > 1000 || files->size() > 100000) {
        return invalidReport("arrays");
    }
    for (const auto& item : *findings) {
        if (!item.is_object()) return invalidReport("finding");
        auto level = parseFindingLevel(json::getString(item, "level").value_or(""));
        if (!level) return invalidReport("finding level");
        a.findings.push_back(AnalysisFinding{*level, json::displayString(item, "message", {}, 1000),
                                             json::displayString(item, "path", {}, 1100)});
    }
    for (const auto& item : *files) {
        if (!item.is_object()) return invalidReport("file");
        AnalyzedFile file;
        file.archivePath = json::getString(item, "archivePath").value_or("");
        file.installPath = json::getString(item, "installPath").value_or("");
        auto kind = parseFileKind(json::getString(item, "kind").value_or(""));
        const auto size = json::getInt(item, "size");
        file.sha256 = json::getString(item, "sha256").value_or("");
        if (!kind || !size || *size < 0 || !archives::normalizeEntryPath(file.archivePath).ok() ||
            !isSafeRelativePath(file.installPath) || !security::isSha256Hex(file.sha256)) {
            return invalidReport("file entry");
        }
        file.kind = *kind;
        file.size = static_cast<std::uint64_t>(*size);
        if (!file.installPath.empty()) {
            ++a.installCount;
            a.installBytes += file.size;
        }
        a.files.push_back(std::move(file));
    }
    // Never trust a stored "installable" more than the stored findings.
    if (a.hasBlockers()) a.installable = false;
    return r;
}

std::vector<ModCheckReport> loadReportsForTitle(const AppPaths& paths, const std::string& titleId,
                                                const std::string& excludeId) {
    std::vector<ModCheckReport> reports;
    std::error_code ec;
    for (fs::directory_iterator it(paths.cache() / "analysis", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() != 21 || name.substr(16) != ".json") continue;
        const std::string id = name.substr(0, 16);
        if (id == excludeId) continue;
        auto loaded = loadReport(paths, id);
        if (loaded && loaded->titleId == titleId) reports.push_back(std::move(loaded).value());
        if (reports.size() >= 200) break;
    }
    return reports;
}

void completeReport(ModCheckReport& report, const AppPaths& paths) {
    const auto others = loadReportsForTitle(paths, report.titleId, report.downloadId);
    std::vector<OtherMod> refs;
    refs.reserve(others.size());
    for (const auto& other : others) refs.push_back(OtherMod{other.downloadId, other.displayName, &other.analysis});
    report.conflicts = predictConflicts(report.analysis, refs);
    // A plan needs a game; downloads not tied to an installed title have none.
    if (games::isValidTitleId(report.titleId)) {
        report.plan = planInstall(report.analysis, paths, report.titleId, report.downloadId);
    } else {
        report.plan.reset();
    }
}

}  // namespace akeno::mods
