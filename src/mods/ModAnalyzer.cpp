// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/mods/ModAnalyzer.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"

namespace akeno::mods {

using providers::CompatibilityStatus;

namespace {

std::string lowerExtension(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return strings::toLowerAscii(name.substr(dot + 1));
}

std::string lowerName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return strings::toLowerAscii(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool startsWithBytes(std::string_view head, std::string_view magic) { return head.substr(0, magic.size()) == magic; }

bool oneOf(const std::string& value, std::initializer_list<std::string_view> options) {
    return std::find(options.begin(), options.end(), value) != options.end();
}

std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    for (const auto& part : strings::split(path, '/')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

int rank(CompatibilityStatus status) {
    // Higher is worse; analysis can only make a label worse, never better.
    switch (status) {
        case CompatibilityStatus::Verified: return 0;
        case CompatibilityStatus::Likely: return 1;
        case CompatibilityStatus::Experimental: return 2;
        case CompatibilityStatus::Unknown: return 3;
        case CompatibilityStatus::PcOnly: return 4;
        case CompatibilityStatus::Incompatible: return 5;
    }
    return 3;
}

}  // namespace

std::string_view toString(FileKind kind) noexcept {
    switch (kind) {
        case FileKind::Asset: return "asset";
        case FileKind::Text: return "text";
        case FileKind::Image: return "image";
        case FileKind::Config: return "config";
        case FileKind::Script: return "script";
        case FileKind::Archive: return "archive";
        case FileKind::WindowsCode: return "windows-code";
        case FileKind::NativeCode: return "native-code";
        case FileKind::Junk: return "junk";
    }
    return "asset";
}

std::string_view toString(FindingLevel level) noexcept {
    switch (level) {
        case FindingLevel::Info: return "info";
        case FindingLevel::Warning: return "warning";
        case FindingLevel::Blocker: return "blocker";
    }
    return "info";
}

FileKind classifyFile(std::string_view path, std::string_view head) {
    const std::string name = lowerName(path);
    const std::string ext = lowerExtension(path);
    const std::string lowerPath = strings::toLowerAscii(path);
    if (strings::startsWith(lowerPath, "__macosx/") || name == ".ds_store" || name == "thumbs.db" ||
        name == "desktop.ini" || strings::startsWith(name, "._")) {
        return FileKind::Junk;
    }
    // Content first: a renamed executable is still an executable.
    if (startsWithBytes(head, "MZ")) return FileKind::WindowsCode;
    if (startsWithBytes(head, "\x7F" "ELF") || startsWithBytes(head, std::string_view("\x4F\x15\x3D\x1D", 4))) {
        return FileKind::NativeCode;
    }
    if (startsWithBytes(head, "#!")) return FileKind::Script;
    if (oneOf(ext, {"exe", "dll", "asi", "sys", "com", "scr", "msi", "pdb", "cpl", "ocx"})) return FileKind::WindowsCode;
    if (oneOf(ext, {"elf", "self", "prx", "sprx", "so", "dylib"}) || name == "eboot.bin") return FileKind::NativeCode;
    if (oneOf(ext, {"lua", "luac", "py", "pyc", "js", "sh", "bat", "cmd", "ps1", "vbs", "jar"})) return FileKind::Script;
    if (oneOf(ext, {"zip", "7z", "rar", "tar", "gz", "tgz", "xz", "bz2", "zst"})) return FileKind::Archive;
    if (oneOf(ext, {"png", "jpg", "jpeg", "dds", "tga", "bmp", "webp", "gif"})) return FileKind::Image;
    if (oneOf(ext, {"txt", "md", "pdf", "rtf", "htm", "html", "nfo"}) || strings::startsWith(name, "readme") ||
        strings::startsWith(name, "license") || strings::startsWith(name, "changelog")) {
        return FileKind::Text;
    }
    if (oneOf(ext, {"ini", "json", "xml", "cfg", "conf", "yaml", "yml", "toml", "csv"})) return FileKind::Config;
    return FileKind::Asset;
}

bool ModAnalysis::hasBlockers() const {
    return std::any_of(findings.begin(), findings.end(),
                       [](const AnalysisFinding& f) { return f.level == FindingLevel::Blocker; });
}

ModAnalysis analyzeMod(const AnalysisInput& input) {
    ModAnalysis result;
    CompatibilityStatus status = input.catalogueStatus;
    auto worsen = [&](CompatibilityStatus to) {
        if (rank(to) > rank(status)) status = to;
    };
    auto add = [&](FindingLevel level, std::string message, std::string path = {}) {
        result.findings.push_back(AnalysisFinding{level, std::move(message), std::move(path)});
    };

    const std::string root = input.archiveRoot.empty() ? std::string() : input.archiveRoot + "/";
    const std::string prefix = input.targetPrefix.empty() ? std::string() : input.targetPrefix + "/";
    std::map<FileKind, std::pair<std::size_t, std::string>> kinds;  // count, example
    std::map<std::string, std::string> folded;                       // lower-case install path -> first path
    std::size_t outsideRoot = 0;
    std::size_t caseCollisions = 0;
    std::string caseExample;
    bool ue4ss = false;
    std::string loader;  // a PC mod loader the files are made for
    bool fakelib = false;
    bool sceSys = false;
    bool tooLong = false;
    int maxDepth = 0;
    std::size_t paks = 0;
    std::size_t unityFiles = 0;

    for (const auto& file : input.files) {
        AnalyzedFile analyzed;
        analyzed.archivePath = file.path;
        analyzed.size = file.size;
        analyzed.sha256 = file.sha256;
        analyzed.kind = classifyFile(file.path, file.head);
        auto& entry = kinds[analyzed.kind];
        if (entry.first++ == 0) entry.second = file.path;

        const std::string lowerPath = strings::toLowerAscii(file.path);
        if (lowerPath.find("ue4ss") != std::string::npos) ue4ss = true;
        // Loader markers establish an unsupported PC dependency, not the cause of a crash.
        const std::string base = lowerPath.substr(lowerPath.rfind('/') == std::string::npos ? 0 : lowerPath.rfind('/') + 1);
        if (base == "modconfig.json") loader = "Reloaded-II";
        if (base == "modinfo.ini" && loader.empty()) loader = "Fluffy Mod Manager";
        for (const auto& part : components(lowerPath)) {
            if (part == "dsts-loader" || part == "reloaded-ii" || part == "reloaded.mod.loader") {
                loader = "Reloaded-II/dsts-loader";
            }
        }
        const std::string ext = lowerExtension(file.path);
        if (ext == "pak" || ext == "utoc" || ext == "ucas") ++paks;
        if (ext == "assets" || ext == "bundle" || ext == "resource") ++unityFiles;

        const bool insideRoot = root.empty() || strings::startsWith(file.path, root);
        if (!insideRoot) {
            ++outsideRoot;
        } else if (analyzed.kind != FileKind::Junk) {
            analyzed.installPath = prefix + file.path.substr(root.size());
            const auto parts = components(analyzed.installPath);
            const std::string top = parts.empty() ? std::string() : strings::toLowerAscii(parts.front());
            if (top == "fakelib" || top == "fakelib2") fakelib = true;
            if (top == "sce_sys" || top == "sce_module") sceSys = true;
            maxDepth = std::max(maxDepth, static_cast<int>(parts.size()));
            // Room for "/data/homebrew/backports/<TITLE_ID>/" within ShadowMountPlus's path limit.
            if (analyzed.installPath.size() > limits::kMaxPathBytes - 64) tooLong = true;
            auto [it, inserted] = folded.emplace(strings::toLowerAscii(analyzed.installPath), analyzed.installPath);
            if (!inserted && it->second != analyzed.installPath) {
                if (caseCollisions++ == 0) caseExample = analyzed.installPath;
            }
            ++result.installCount;
            result.installBytes += file.size;
        }
        result.files.push_back(std::move(analyzed));
    }

    auto count = [&](FileKind kind) { return kinds.count(kind) != 0 ? kinds[kind].first : std::size_t{0}; };
    auto example = [&](FileKind kind) { return kinds.count(kind) != 0 ? kinds[kind].second : std::string(); };

    // Blockers: content Akeno will never install.
    if (count(FileKind::NativeCode) > 0) {
        worsen(CompatibilityStatus::Incompatible);
        add(FindingLevel::Blocker,
            strings::concat("Contains program code for the console (", count(FileKind::NativeCode),
                            " files). Akeno never installs or runs code."),
            example(FileKind::NativeCode));
    }
    if (count(FileKind::WindowsCode) > 0 || ue4ss) {
        worsen(CompatibilityStatus::PcOnly);
        add(FindingLevel::Blocker,
            ue4ss ? "Needs UE4SS, a Windows-only mod loader. This is a PC mod."
                  : strings::concat("Contains Windows programs or libraries (", count(FileKind::WindowsCode),
                                    " files). This is a PC mod."),
            ue4ss ? std::string() : example(FileKind::WindowsCode));
    }
    if (!loader.empty()) {
        worsen(CompatibilityStatus::PcOnly);
        add(FindingLevel::Blocker,
            "Contains markers for " + loader + ", an unsupported PC mod loader. Copying these files into a "
            "PS5 overlay does not provide that loader or establish data-format compatibility.");
    }
    if (fakelib) {
        worsen(CompatibilityStatus::Incompatible);
        add(FindingLevel::Blocker,
            "Contains a fakelib folder, which ShadowMountPlus loads as system libraries. Akeno refuses it.");
    }
    if (sceSys) {
        worsen(CompatibilityStatus::Incompatible);
        add(FindingLevel::Blocker, "Changes the game's system files (sce_sys or sce_module). Akeno refuses this.");
    }
    if (tooLong) {
        add(FindingLevel::Blocker, "A file path is too long for ShadowMountPlus.");
    }
    if (result.installCount == 0) {
        add(FindingLevel::Blocker,
            outsideRoot > 0 ? "No file is inside the folder the catalogue names (archiveRoot). Nothing would be installed."
                            : "The archive contains nothing to install.");
    }
    if (input.sourceType == games::SourceType::Pkg) {
        if (maxDepth > 64) {
            add(FindingLevel::Blocker, "The folders are nested deeper than ShadowMountPlus allows for installed packages (64).");
        }
        if (result.installCount > 256) {
            add(FindingLevel::Warning,
                strings::concat("This mod has ", result.installCount,
                                " files. Installed packages allow at most 256 redirected files or new folders; the "
                                "exact count is checked before installing."));
        }
    }

    // Warnings.
    if (count(FileKind::Script) > 0) {
        add(FindingLevel::Warning,
            strings::concat("Contains scripts (", count(FileKind::Script),
                            " files). Akeno never runs them; they only work if the game loads them itself."),
            example(FileKind::Script));
    }
    if (count(FileKind::Archive) > 0) {
        add(FindingLevel::Warning,
            strings::concat("Contains other archives (", count(FileKind::Archive),
                            " files). They are installed as they are, not unpacked."),
            example(FileKind::Archive));
    }
    if (caseCollisions > 0) {
        add(FindingLevel::Warning,
            strings::concat(caseCollisions, " file names differ only in upper/lower case."), caseExample);
    }

    // Information.
    if (outsideRoot > 0) {
        add(FindingLevel::Info, strings::concat(outsideRoot, " files outside the mod folder are not installed (for "
                                                             "example a readme)."));
    }
    if (count(FileKind::Junk) > 0) {
        add(FindingLevel::Info, strings::concat(count(FileKind::Junk), " system leftover files (__MACOSX, .DS_Store) "
                                                                       "are ignored."));
    }
    if (paks > 0) result.engineHint = "Unreal Engine packages";
    else if (unityFiles > 0) result.engineHint = "Unity assets";

    std::stable_sort(result.findings.begin(), result.findings.end(),
                     [](const AnalysisFinding& a, const AnalysisFinding& b) {
                         return static_cast<int>(a.level) > static_cast<int>(b.level);
                     });
    result.status = status;
    result.installable = !result.hasBlockers() && input.catalogueInstallable &&
                         (status == CompatibilityStatus::Verified || status == CompatibilityStatus::Likely ||
                          status == CompatibilityStatus::Experimental);
    return result;
}

std::vector<Conflict> predictConflicts(const ModAnalysis& mod, const std::vector<OtherMod>& others) {
    std::set<std::string> mine;
    for (const auto& file : mod.files) {
        if (!file.installPath.empty()) mine.insert(strings::toLowerAscii(file.installPath));
    }
    std::vector<Conflict> conflicts;
    for (const auto& other : others) {
        if (other.analysis == nullptr) continue;
        Conflict conflict;
        conflict.otherId = other.id;
        conflict.otherName = other.name;
        for (const auto& file : other.analysis->files) {
            if (file.installPath.empty() || mine.count(strings::toLowerAscii(file.installPath)) == 0) continue;
            if (conflict.paths.size() < 20) conflict.paths.push_back(file.installPath);
            ++conflict.count;
        }
        if (conflict.count > 0) conflicts.push_back(std::move(conflict));
    }
    return conflicts;
}

InstallPlan planInstall(const ModAnalysis& analysis, const AppPaths& paths, const std::string& titleId,
                        const std::string& downloadId, std::optional<bool> hardLinks) {
    InstallPlan plan;
    plan.titleId = titleId;
    plan.modStore = paths.mods() / titleId / downloadId;
    plan.overlayNext = paths.overlays() / titleId / "overlay.next";
    plan.backportDirectory = "/data/homebrew/backports/" + titleId;
    plan.files = analysis.installCount;
    plan.bytes = analysis.installBytes;
    plan.changesGameFiles = false;
    plan.executable = analysis.installable;
    if (analysis.hasBlockers()) {
        plan.notExecutableReason = "The checks found problems that block installing.";
    } else if (!analysis.installable) {
        plan.notExecutableReason = "Its compatibility label does not allow installing.";
    }
    for (const auto& file : analysis.files) {
        if (file.installPath.empty()) continue;
        if (plan.mapping.size() >= 200) break;
        plan.mapping.emplace_back(file.archivePath, plan.backportDirectory + "/" + file.installPath);
    }
    std::string overlayStep = "Create " + plan.overlayNext.string();
    if (hardLinks == true) {
        overlayStep += " with hard links to those files and to the other enabled mods of this game, in load order. "
                       "Links need no extra space.";
    } else if (hardLinks == false) {
        plan.overlayExtraBytes = plan.bytes;
        overlayStep += " with copies of those files and of the other enabled mods of this game, in load order. Hard "
                       "links do not work in Akeno's folder on this console, so this needs another " +
                       strings::formatBytes(plan.bytes) + ".";
    } else {
        plan.overlayExtraBytes = plan.bytes;
        overlayStep += " from those files and the other enabled mods of this game, in load order: hard links where "
                       "the console supports them, otherwise copies (up to " +
                       strings::formatBytes(plan.bytes) + " more).";
    }
    plan.steps = {
        {"Keep the mod's files",
         strings::concat("Copy ", plan.files, " files (", strings::formatBytes(plan.bytes), ") into ",
                         plan.modStore.string(), ".")},
        {"Build the overlay", overlayStep},
        {"Check the overlay",
         "Only plain files and folders; no fakelib or system folders; every file matches its SHA-256; within "
         "ShadowMountPlus limits."},
        {"Switch overlays safely",
         "Rename the current overlay to overlay.previous, then rename overlay.next to " + plan.backportDirectory +
             ". Each step is written to the recovery journal first."},
        {"Play",
         "ShadowMountPlus applies the overlay the next time the game starts. The original game files are never changed, "
         "and removing the overlay restores the unmodified game."},
    };
    return plan;
}

}  // namespace akeno::mods
