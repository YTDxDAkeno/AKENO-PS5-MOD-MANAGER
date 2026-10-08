// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/mods/ModCheck.hpp"

#include <system_error>

#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::mods {

namespace fs = std::filesystem;
using json::Json;
using logging::logger;
using providers::CompatibilityStatus;

namespace {

constexpr std::size_t kMaxReportBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxListItems = 500;
constexpr std::size_t kMaxTextBytes = 2000;

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

Json listJson(const std::vector<std::string>& items, std::size_t limit = kMaxListItems) {
    Json out = Json::array();
    for (std::size_t i = 0; i < items.size() && i < limit; ++i) out.push_back(items[i]);
    return out;
}

// Display text only: sanitized, bounded, at most kMaxListItems entries.
std::vector<std::string> listFrom(const Json& object, std::string_view key) {
    std::vector<std::string> out;
    const Json* array = json::getArray(object, key);
    if (array == nullptr) return out;
    for (const auto& item : *array) {
        if (out.size() >= kMaxListItems) break;
        if (item.is_string()) out.push_back(strings::sanitizeForDisplay(item.get<std::string>(), kMaxTextBytes));
    }
    return out;
}

Json layoutJson(const ArchiveLayout& layout) {
    Json ignored = Json::array();
    for (const auto& [path, reason] : layout.ignored) {
        if (ignored.size() >= kMaxListItems) break;
        ignored.push_back({{"path", path}, {"reason", reason}});
    }
    return {{"rule", layout.rule},
            {"confidence", std::string(toString(layout.confidence))},
            {"topLevel", listJson(layout.topLevel)},
            {"packagingFolders", listJson(layout.packagingFolders)},
            {"archiveRoot", layout.archiveRoot},
            {"targetPrefix", layout.targetPrefix},
            {"anchor", layout.anchor},
            {"evidence", listJson(layout.evidence)},
            {"problems", listJson(layout.problems)},
            {"ignored", std::move(ignored)}};
}

std::optional<ArchiveLayout> layoutFrom(const Json& d) {
    ArchiveLayout layout;
    layout.rule = json::displayString(d, "rule", "none", 32);
    auto confidence = parseMappingConfidence(json::getString(d, "confidence").value_or(""));
    if (!confidence) return std::nullopt;
    layout.confidence = *confidence;
    layout.topLevel = listFrom(d, "topLevel");
    layout.packagingFolders = listFrom(d, "packagingFolders");
    layout.archiveRoot = json::getString(d, "archiveRoot").value_or("");
    layout.targetPrefix = json::getString(d, "targetPrefix").value_or("");
    layout.anchor = json::displayString(d, "anchor", {}, kMaxTextBytes);
    if ((!layout.archiveRoot.empty() && !isSafeRelativePath(layout.archiveRoot)) ||
        (!layout.targetPrefix.empty() && !isSafeRelativePath(layout.targetPrefix))) {
        return std::nullopt;
    }
    layout.evidence = listFrom(d, "evidence");
    layout.problems = listFrom(d, "problems");
    if (const Json* ignored = json::getArray(d, "ignored")) {
        for (const auto& item : *ignored) {
            if (!item.is_object() || layout.ignored.size() >= kMaxListItems) continue;
            layout.ignored.emplace_back(json::displayString(item, "path", {}, kMaxTextBytes),
                                        json::displayString(item, "reason", {}, 200));
        }
    }
    return layout;
}

Json assessmentJson(const compatibility::Assessment& a) {
    Json evidence = Json::array();
    for (const auto& item : a.evidence) {
        if (evidence.size() >= kMaxListItems) break;
        evidence.push_back({{"kind", std::string(compatibility::toString(item.kind))},
                            {"source", std::string(compatibility::toString(item.source))},
                            {"text", item.text}});
    }
    return {{"outcome", std::string(compatibility::toString(a.outcome))},
            {"category", std::string(compatibility::toString(a.category))},
            {"mappingConfidence", std::string(toString(a.mappingConfidence))},
            {"platform", std::string(compatibility::toString(a.platform))},
            {"loading", std::string(compatibility::toString(a.loading))},
            {"activationAllowed", a.activationAllowed},
            {"needsConfirmation", a.needsConfirmation},
            {"blockedReasons", listJson(a.blockedReasons)},
            {"risks", listJson(a.risks)},
            {"evidence", std::move(evidence)},
            {"engine", a.engine},
            {"modFormat", a.modFormat},
            {"pcDependencies", listJson(a.pcDependencies)},
            {"conversions", listJson(a.conversions)},
            {"adapter", a.adapter},
            {"region", a.region},
            {"summary", a.summary}};
}

std::optional<compatibility::Assessment> assessmentFrom(const Json& d) {
    compatibility::Assessment a;
    auto outcome = compatibility::parseOutcome(json::getString(d, "outcome").value_or(""));
    auto category = compatibility::parseCategory(json::getString(d, "category").value_or(""));
    auto mapping = parseMappingConfidence(json::getString(d, "mappingConfidence").value_or(""));
    auto platform = compatibility::parsePlatformCompatibility(json::getString(d, "platform").value_or(""));
    auto loading = compatibility::parseLoadingSupport(json::getString(d, "loading").value_or(""));
    if (!outcome || !category || !mapping || !platform || !loading) return std::nullopt;
    a.outcome = *outcome;
    a.category = *category;
    a.mappingConfidence = *mapping;
    a.platform = *platform;
    a.loading = *loading;
    a.blockedReasons = listFrom(d, "blockedReasons");
    // A stored "allowed" never outweighs stored reasons against it.
    a.activationAllowed = json::getBool(d, "activationAllowed").value_or(false) && a.blockedReasons.empty();
    a.needsConfirmation = json::getBool(d, "needsConfirmation").value_or(false);
    a.risks = listFrom(d, "risks");
    if (const Json* evidence = json::getArray(d, "evidence")) {
        for (const auto& item : *evidence) {
            if (!item.is_object() || a.evidence.size() >= kMaxListItems) continue;
            compatibility::Evidence e;
            const std::string kind = json::getString(item, "kind").value_or("");
            const std::string source = json::getString(item, "source").value_or("");
            e.kind = kind == "supports" ? compatibility::EvidenceKind::Supports
                     : kind == "against" ? compatibility::EvidenceKind::Against
                                         : compatibility::EvidenceKind::Limitation;
            for (auto s : {compatibility::EvidenceSource::Archive, compatibility::EvidenceSource::Container,
                           compatibility::EvidenceSource::Game, compatibility::EvidenceSource::Catalogue,
                           compatibility::EvidenceSource::Adapter, compatibility::EvidenceSource::Rules}) {
                if (compatibility::toString(s) == source) e.source = s;
            }
            e.text = json::displayString(item, "text", {}, kMaxTextBytes);
            a.evidence.push_back(std::move(e));
        }
    }
    a.engine = json::displayString(d, "engine", {}, 100);
    a.modFormat = json::displayString(d, "modFormat", {}, 300);
    a.pcDependencies = listFrom(d, "pcDependencies");
    a.conversions = listFrom(d, "conversions");
    a.adapter = json::displayString(d, "adapter", {}, 100);
    a.region = json::displayString(d, "region", {}, 100);
    a.summary = json::displayString(d, "summary", {}, kMaxTextBytes);
    return a;
}

Json unrealJson(const UnrealReport& u) {
    Json sets = Json::array();
    for (const auto& set : u.sets) {
        sets.push_back({{"name", set.name},
                        {"kind", set.kind},
                        {"members", listJson(set.members)},
                        {"companionsVerified", set.companionsVerified},
                        {"tocVersion", set.tocVersion},
                        {"pakVersion", set.pakVersion},
                        {"containerId", set.containerId},
                        {"containerFlags", set.containerFlags},
                        {"packages", listJson(set.packages)},
                        {"issues", listJson(set.issues)},
                        {"unknowns", listJson(set.unknowns)},
                        {"evidence", listJson(set.evidence)}});
        if (sets.size() >= 64) break;
    }
    return {{"detected", u.detected},
            {"format", u.format},
            {"sets", std::move(sets)},
            {"loaders", listJson(u.loaders)},
            {"modFolders", listJson(u.modFolders)},
            {"containedPackages", listJson(u.containedPackages)},
            {"importedPackages", listJson(u.importedPackages, 200)},
            {"importedTotal", u.importedTotal},
            {"looseAssets", u.looseAssets},
            {"configFiles", u.configFiles},
            {"maxTocVersion", u.maxTocVersion},
            {"maxPakVersion", u.maxPakVersion},
            {"unversioned", u.unversioned},
            {"compatibilityUnknown", u.compatibilityUnknown}};
}

UnrealReport unrealFrom(const Json& d) {
    UnrealReport u;
    u.detected = json::getBool(d, "detected").value_or(false);
    u.format = json::displayString(d, "format", {}, 300);
    if (const Json* sets = json::getArray(d, "sets")) {
        for (const auto& item : *sets) {
            if (!item.is_object() || u.sets.size() >= 64) continue;
            UnrealSetReport set;
            set.name = json::displayString(item, "name", {}, kMaxTextBytes);
            set.kind = json::displayString(item, "kind", {}, 100);
            set.members = listFrom(item, "members");
            set.companionsVerified = json::getBool(item, "companionsVerified").value_or(false);
            set.tocVersion = static_cast<int>(json::getInt(item, "tocVersion").value_or(0));
            set.pakVersion = static_cast<int>(json::getInt(item, "pakVersion").value_or(0));
            set.containerId = json::displayString(item, "containerId", {}, 32);
            set.containerFlags = json::displayString(item, "containerFlags", {}, 100);
            set.packages = listFrom(item, "packages");
            set.issues = listFrom(item, "issues");
            set.unknowns = listFrom(item, "unknowns");
            set.evidence = listFrom(item, "evidence");
            u.sets.push_back(std::move(set));
        }
    }
    u.loaders = listFrom(d, "loaders");
    u.modFolders = listFrom(d, "modFolders");
    u.containedPackages = listFrom(d, "containedPackages");
    u.importedPackages = listFrom(d, "importedPackages");
    u.importedTotal = static_cast<std::size_t>(std::max<std::int64_t>(0, json::getInt(d, "importedTotal").value_or(0)));
    u.looseAssets = static_cast<std::size_t>(std::max<std::int64_t>(0, json::getInt(d, "looseAssets").value_or(0)));
    u.configFiles = static_cast<std::size_t>(std::max<std::int64_t>(0, json::getInt(d, "configFiles").value_or(0)));
    u.maxTocVersion = static_cast<int>(json::getInt(d, "maxTocVersion").value_or(0));
    u.maxPakVersion = static_cast<int>(json::getInt(d, "maxPakVersion").value_or(0));
    u.unversioned = json::getBool(d, "unversioned").value_or(false);
    u.compatibilityUnknown = json::getBool(d, "compatibilityUnknown").value_or(false);
    return u;
}

Json gameJson(const GameReport& g) {
    return {{"version", g.version},
            {"contentId", g.contentId},
            {"region", g.region},
            {"listed", g.listed},
            {"complete", g.complete},
            {"reason", g.reason},
            {"entries", g.entries},
            {"topLevel", listJson(g.topLevel, 100)},
            {"paksDirectory", g.paksDirectory},
            {"containers", listJson(g.containers, 40)},
            {"maxTocVersion", g.maxTocVersion},
            {"maxPakVersion", g.maxPakVersion},
            {"signedContainers", g.signedContainers},
            {"encryptedContainers", g.encryptedContainers},
            {"packageIdsComplete", g.packageIdsComplete},
            {"packageIds", g.packageIds}};
}

GameReport gameFrom(const Json& d) {
    GameReport g;
    g.version = json::displayString(d, "version", {}, 64);
    g.contentId = json::displayString(d, "contentId", {}, 64);
    g.region = json::displayString(d, "region", {}, 64);
    g.listed = json::getBool(d, "listed").value_or(false);
    g.complete = json::getBool(d, "complete").value_or(false);
    g.reason = json::displayString(d, "reason", {}, kMaxTextBytes);
    g.entries = static_cast<std::size_t>(std::max<std::int64_t>(0, json::getInt(d, "entries").value_or(0)));
    g.topLevel = listFrom(d, "topLevel");
    g.paksDirectory = json::displayString(d, "paksDirectory", {}, kMaxTextBytes);
    g.containers = listFrom(d, "containers");
    g.maxTocVersion = static_cast<int>(json::getInt(d, "maxTocVersion").value_or(0));
    g.maxPakVersion = static_cast<int>(json::getInt(d, "maxPakVersion").value_or(0));
    g.signedContainers = json::getBool(d, "signedContainers").value_or(false);
    g.encryptedContainers = json::getBool(d, "encryptedContainers").value_or(false);
    g.packageIdsComplete = json::getBool(d, "packageIdsComplete").value_or(false);
    g.packageIds = static_cast<std::size_t>(std::max<std::int64_t>(0, json::getInt(d, "packageIds").value_or(0)));
    return g;
}

std::string hex16(std::uint64_t id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kHex[id & 0xF];
        id >>= 4;
    }
    return text;
}

std::string tocFlags(std::uint8_t flags) {
    std::string text;
    for (auto [bit, name] : {std::pair<int, const char*>{1, "compressed"}, {2, "encrypted"}, {4, "signed"}, {8, "indexed"}}) {
        if ((flags & bit) != 0) text += text.empty() ? name : std::string(", ") + name;
    }
    return text.empty() ? "none" : text;
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
        case CheckPhase::Analysing: return "Comparing the mod with the installed game";
        case CheckPhase::CleaningUp: return "Deleting the staging folder";
    }
    return "";
}

UnrealReport summarizeUnreal(const unreal::UnrealAnalysis& analysis) {
    UnrealReport u;
    u.detected = analysis.detected;
    u.format = analysis.format;
    for (const auto& set : analysis.sets) {
        UnrealSetReport s;
        s.name = set.directory.empty() ? set.stem : set.directory + "/" + set.stem;
        s.kind = std::string(unreal::toString(set.kind));
        s.members = set.members;
        s.companionsVerified = set.companionsVerified;
        if (set.toc && set.toc->status == unreal::ParseStatus::Parsed) {
            s.tocVersion = set.toc->version;
            s.containerId = hex16(set.toc->containerId);
            s.containerFlags = tocFlags(set.toc->flags);
        }
        if (set.pak && set.pak->status == unreal::ParseStatus::Parsed) s.pakVersion = set.pak->version;
        for (const auto& package : set.packages) s.packages.push_back(package.name);
        s.issues = set.issues;
        s.unknowns = set.unknowns;
        s.evidence = set.evidence;
        u.sets.push_back(std::move(s));
    }
    u.loaders = analysis.loaders;
    u.modFolders = analysis.modFolders;
    u.containedPackages = analysis.containedPackages;
    u.importedTotal = analysis.importedPackages.size();
    for (std::size_t i = 0; i < analysis.importedPackages.size() && i < 200; ++i) {
        u.importedPackages.push_back(analysis.importedPackages[i]);
    }
    u.looseAssets = analysis.looseAssets.size();
    u.configFiles = analysis.configFiles.size();
    u.maxTocVersion = analysis.maxTocVersion;
    u.maxPakVersion = analysis.maxPakVersion;
    u.unversioned = analysis.unversionedPackages;
    u.compatibilityUnknown = analysis.compatibilityUnknown;
    return u;
}

GameReport summarizeGame(const Preparation& p, const std::string& version, const std::string& contentId) {
    GameReport g;
    g.version = version;
    g.contentId = contentId;
    g.region = compatibility::regionFromContentId(contentId);
    g.listed = p.gameTree.available;
    g.complete = p.gameTree.complete;
    g.reason = p.gameTree.reason;
    g.entries = p.gameTree.entries.size();
    g.topLevel = p.gameTree.topLevel();
    g.paksDirectory = p.gameUnreal.paksDirectory;
    for (const auto& container : p.gameUnreal.containers) {
        if (g.containers.size() >= 40) break;
        std::string text = container.path + ": ";
        if (container.status != unreal::ParseStatus::Parsed) {
            text += std::string(unreal::toString(container.status)) + (container.detail.empty() ? "" : " (" + container.detail + ")");
        } else if (container.kind == "utoc") {
            text += strings::concat("IoStore version ", container.version, ", flags ", tocFlags(container.flags), ", ",
                                    container.entries, " chunks");
        } else {
            text += strings::concat(".pak version ", container.version, container.encryptedIndex ? ", encrypted index" : "");
        }
        g.containers.push_back(std::move(text));
    }
    g.maxTocVersion = p.gameUnreal.maxTocVersion;
    g.maxPakVersion = p.gameUnreal.maxPakVersion;
    g.signedContainers = p.gameUnreal.anySignedContainer || p.gameUnreal.signatureFiles;
    g.encryptedContainers = p.gameUnreal.anyEncrypted;
    g.packageIdsComplete = p.gameUnreal.packageIdsComplete;
    g.packageIds = p.gameUnreal.packageIds.size();
    return g;
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
    PreparationRequest prep;
    prep.files = tree->files;
    prep.extractedRoot = staging;
    prep.game = GameContext{request.titleId, request.gameVersion, request.contentId, request.sourceType,
                            request.installedPkg, request.gameFolder};
    prep.provider = request.mod.providerId;
    prep.modId = request.mod.modId;
    prep.modVersion = request.modVersion;
    prep.pcSource = request.pcSource || isPcProvider(request.mod.providerId);
    prep.curated = (request.curated || isCuratedProvider(request.mod.providerId)) && !prep.pcSource;
    if (prep.curated) prep.manifestArchiveRoot = request.archiveRoot;
    prep.manifestTargetPrefix = request.targetPrefix;
    prep.catalogueStatus = request.catalogueStatus;
    prep.catalogueInstallable = request.catalogueInstallable;
    prep.installedMods = request.installedMods;
    prep.registry = request.registry;
    prep.cancel = cancel;
    Preparation prepared = prepareMod(prep);
    if (cancel != nullptr && cancel->cancelled()) {
        return finish(makeError(ErrorCode::Cancelled, "The check was cancelled."));
    }

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
    result.analysis = std::move(prepared.analysis);
    result.layout = prepared.layout;
    result.layout.mapping.clear();
    result.unreal = summarizeUnreal(prepared.unreal);
    result.game = summarizeGame(prepared, request.gameVersion, request.contentId);
    result.assessment = prepared.assessment;
    result.installedConflicts = prepared.installedConflicts;
    logger().info("check", strings::concat(request.downloadId, ": ", result.analysis.installCount, " files to install, ",
                                           result.analysis.findings.size(), " findings, label ",
                                           toString(result.analysis.status), "; ", result.assessment.summary));
    return finish(std::move(result));
}

fs::path reportPath(const AppPaths& paths, const std::string& downloadId) {
    return paths.cache() / "analysis" / (downloadId + ".json");
}

Status saveReport(const security::SafeFs& fs, const AppPaths& paths, const ModCheckReport& report) {
    Json document = Json::object();
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
    Json findings = Json::array();
    for (const auto& finding : a.findings) {
        findings.push_back({{"level", std::string(toString(finding.level))},
                            {"message", finding.message},
                            {"path", finding.path}});
    }
    document["findings"] = std::move(findings);
    Json files = Json::array();
    for (const auto& file : a.files) {
        files.push_back({{"archivePath", file.archivePath},
                         {"installPath", file.installPath},
                         {"size", file.size},
                         {"sha256", file.sha256},
                         {"kind", std::string(toString(file.kind))},
                         {"target", std::string(toString(file.target))}});
    }
    document["files"] = std::move(files);
    document["layout"] = layoutJson(report.layout);
    document["unreal"] = unrealJson(report.unreal);
    document["game"] = gameJson(report.game);
    document["assessment"] = assessmentJson(report.assessment);
    AKENO_TRY(fs.createDirectories(paths.cache() / "analysis"));
    return fs.writeFileAtomic(reportPath(paths, report.downloadId),
                              document.dump(1, ' ', false, Json::error_handler_t::replace));
}

Result<ModCheckReport> loadReport(const AppPaths& paths, const std::string& downloadId) {
    if (!isValidDownloadId(downloadId)) return invalidReport("bad id");
    auto text = security::readFileBounded(reportPath(paths, downloadId), kMaxReportBytes);
    if (!text) return std::move(text).error();
    auto parsed = json::parseBounded(text.value(), kMaxReportBytes);
    if (!parsed) return std::move(parsed).error();
    const Json& d = parsed.value();
    if (!d.is_object() || json::getInt(d, "schemaVersion") != ModCheckReport::kSchemaVersion ||
        json::getString(d, "downloadId") != downloadId) {
        return invalidReport("schema or id");
    }
    ModCheckReport r;
    r.downloadId = downloadId;
    if (const Json* mod = json::getObject(d, "mod")) {
        r.mod.providerId = json::displayString(*mod, "provider");
        r.mod.modId = json::displayString(*mod, "id");
    }
    r.displayName = json::displayString(d, "name");
    r.modVersion = json::displayString(d, "version");
    r.titleId = json::displayString(d, "titleId");
    r.checkedAt = json::displayString(d, "checkedAt");
    if (const Json* archive = json::getObject(d, "archive")) {
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
    const Json* findings = json::getArray(d, "findings");
    const Json* files = json::getArray(d, "files");
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
        auto target = parseTargetState(json::getString(item, "target").value_or("unknown"));
        const auto size = json::getInt(item, "size");
        file.sha256 = json::getString(item, "sha256").value_or("");
        if (!kind || !target || !size || *size < 0 || !archives::normalizeEntryPath(file.archivePath).ok() ||
            !isSafeRelativePath(file.installPath) || !security::isSha256Hex(file.sha256)) {
            return invalidReport("file entry");
        }
        file.kind = *kind;
        file.target = *target;
        file.size = static_cast<std::uint64_t>(*size);
        if (!file.installPath.empty()) {
            ++a.installCount;
            a.installBytes += file.size;
        }
        a.files.push_back(std::move(file));
    }
    // Never trust a stored "installable" more than the stored findings.
    if (a.hasBlockers()) a.installable = false;

    const Json* layout = json::getObject(d, "layout");
    const Json* assessment = json::getObject(d, "assessment");
    if (layout == nullptr || assessment == nullptr) return invalidReport("layout or assessment");
    auto parsedLayout = layoutFrom(*layout);
    auto parsedAssessment = assessmentFrom(*assessment);
    if (!parsedLayout || !parsedAssessment) return invalidReport("layout or assessment fields");
    r.layout = std::move(parsedLayout).value();
    for (const auto& file : a.files) {
        if (!file.installPath.empty()) r.layout.mapping.emplace_back(file.archivePath, file.installPath);
    }
    r.assessment = std::move(parsedAssessment).value();
    if (a.hasBlockers()) r.assessment.activationAllowed = false;
    if (const Json* unrealDoc = json::getObject(d, "unreal")) r.unreal = unrealFrom(*unrealDoc);
    if (const Json* gameDoc = json::getObject(d, "game")) r.game = gameFrom(*gameDoc);
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

void completeReport(ModCheckReport& report, const AppPaths& paths, std::optional<bool> hardLinks,
                    const std::vector<InstalledModPaths>& installed) {
    const auto others = loadReportsForTitle(paths, report.titleId, report.downloadId);
    std::vector<OtherMod> refs;
    refs.reserve(others.size());
    for (const auto& other : others) refs.push_back(OtherMod{other.downloadId, other.displayName, &other.analysis});
    report.conflicts = predictConflicts(report.analysis, refs);
    if (!installed.empty()) {
        std::vector<ModAnalysis> installedAnalyses;
        installedAnalyses.reserve(installed.size());
        for (const auto& mod : installed) {
            ModAnalysis analysis;
            for (const auto& path : mod.installPaths) analysis.files.push_back(AnalyzedFile{path, path, 0, {}, FileKind::Asset, TargetState::Unknown});
            installedAnalyses.push_back(std::move(analysis));
        }
        std::vector<OtherMod> installedRefs;
        for (std::size_t i = 0; i < installed.size(); ++i) {
            if (installed[i].id == report.downloadId) continue;
            installedRefs.push_back(OtherMod{installed[i].id, installed[i].name, &installedAnalyses[i]});
        }
        report.installedConflicts = predictConflicts(report.analysis, installedRefs);
    }
    // A plan needs a game; downloads not tied to an installed title have none.
    if (games::isValidTitleId(report.titleId)) {
        PlanContext context;
        context.layout = &report.layout;
        context.assessment = &report.assessment;
        context.installedConflicts = report.installedConflicts;
        context.reserveBytes = limits::kStorageSafetyReserveBytes;
        report.plan = planInstall(report.analysis, paths, report.titleId, report.downloadId, hardLinks, &context);
    } else {
        report.plan.reset();
    }
}

}  // namespace akeno::mods
