// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/games/GameTree.hpp"

#include <algorithm>
#include <cerrno>
#include <functional>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akeno/core/Strings.hpp"
#include "akeno/security/PathGuard.hpp"
#include "akeno/security/SafeOpen.hpp"

namespace akeno::games {

namespace {

bool under(std::string_view root, std::string_view path) {
    return path == root || (path.size() > root.size() && path.substr(0, root.size()) == root && path[root.size()] == '/');
}

}  // namespace

void GameTree::index() {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    lookup_.clear();
    caseCollisions = 0;
    lowerCaseOnly_ = !entries.empty();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string folded = strings::toLowerAscii(entries[i].path);
        if (folded != entries[i].path) lowerCaseOnly_ = false;
        if (!lookup_.emplace(folded, i).second) ++caseCollisions;
    }
}

const GameTreeEntry* GameTree::find(std::string_view path) const {
    auto it = lookup_.find(strings::toLowerAscii(path));
    return it == lookup_.end() ? nullptr : &entries[it->second];
}

std::string GameTree::respell(std::string_view path) const {
    std::string result;
    std::string prefix;
    bool existing = true;
    for (const auto& part : strings::split(path, '/')) {
        if (part.empty()) continue;
        const std::string candidate = prefix.empty() ? part : prefix + "/" + part;
        const GameTreeEntry* entry = existing ? find(candidate) : nullptr;
        if (entry == nullptr) existing = false;
        const std::string spelled = entry != nullptr ? entry->path.substr(entry->path.rfind('/') == std::string::npos
                                                                             ? 0
                                                                             : entry->path.rfind('/') + 1)
                                                     : part;
        result = result.empty() ? spelled : result + "/" + spelled;
        prefix = entry != nullptr ? entry->path : candidate;
    }
    return result;
}

std::vector<std::string> GameTree::topLevel() const {
    std::vector<std::string> names;
    for (const auto& entry : entries) {
        if (entry.path.find('/') == std::string::npos) names.push_back(entry.path);
    }
    return names;
}

bool isListableGameFolder(std::string_view path) {
    auto normalized = security::normalizeAbsolute(path);
    if (!normalized || normalized->string() != path || path == "/") return false;
    for (std::string_view runtime : {"/system_ex", "/system", "/mnt/shadowmnt", "/mnt/sandbox", "/user/app", "/dev",
                                     "/preinst", "/update"}) {
        if (under(runtime, path)) return false;
    }
    return path.find("/backports/") == std::string_view::npos && !strings::endsWith(path, "/backports");
}

GameTree probeGameTree(const std::filesystem::path& root, const GameTreeLimits& limits, const CancellationToken* cancel) {
    GameTree tree;
    tree.root = root.string();
    if (!isListableGameFolder(tree.root)) {
        tree.reason = "Not a physical game folder Akeno may read.";
        return tree;
    }
    security::UniqueFd rootFd = security::openNoFollow(root, true);
    if (!rootFd.valid()) {
        tree.reason = "The game folder cannot be opened: " + security::describeErrno(errno);
        return tree;
    }
    struct stat rootInfo {};
    if (::fstat(rootFd.get(), &rootInfo) != 0) {
        tree.reason = "The game folder cannot be inspected: " + security::describeErrno(errno);
        return tree;
    }
    tree.available = true;
    tree.complete = true;
    const auto started = std::chrono::steady_clock::now();
    auto stop = [&](std::string why) {
        if (tree.complete) tree.reason = std::move(why);
        tree.complete = false;
    };
    std::function<void(int, const std::string&, int)> walk = [&](int directory, const std::string& prefix, int depth) {
        if ((cancel != nullptr && cancel->cancelled()) || std::chrono::steady_clock::now() - started > limits.maxDuration) {
            stop("The listing was stopped by its time limit or cancelled.");
            return;
        }
        if (depth > limits.maxDepth) {
            stop("Folders are nested deeper than the listing limit.");
            return;
        }
        const int copy = security::duplicateDescriptor(directory);
        DIR* dir = copy >= 0 ? ::fdopendir(copy) : nullptr;
        if (dir == nullptr) {
            if (copy >= 0) ::close(copy);
            stop("A folder could not be listed (" + prefix + "): " + security::describeErrno(errno));
            return;
        }
        std::vector<std::string> names;
        for (;;) {
            errno = 0;
            const dirent* entry = ::readdir(dir);
            if (entry == nullptr) {
                if (errno != 0) stop("A folder listing failed (" + prefix + "): " + security::describeErrno(errno));
                break;
            }
            const std::string_view name = entry->d_name;
            if (name != "." && name != "..") names.emplace_back(name);
        }
        ::closedir(dir);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            if (tree.entries.size() >= limits.maxEntries) {
                stop("The game has more files than the listing limit.");
                return;
            }
            struct stat info {};
            if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                stop("An entry could not be inspected (" + prefix + name + "): " + security::describeErrno(errno));
                continue;
            }
            const std::string path = prefix + name;
            if (S_ISREG(info.st_mode)) {
                tree.entries.push_back({path, false, static_cast<std::uint64_t>(std::max<off_t>(0, info.st_size))});
            } else if (S_ISDIR(info.st_mode)) {
                tree.entries.push_back({path, true, 0});
                if (info.st_dev != rootInfo.st_dev) {
                    stop("A nested filesystem was not entered (" + path + ").");
                    continue;
                }
                security::UniqueFd child = security::openBelow(directory, name, true);
                if (!child.valid()) {
                    stop("A folder could not be opened (" + path + "): " + security::describeErrno(errno));
                    continue;
                }
                walk(child.get(), path + "/", depth + 1);
            }
            // Links and special files are not part of the listing and never followed.
        }
    };
    walk(rootFd.get(), "", 0);
    tree.index();
    return tree;
}

}  // namespace akeno::games
