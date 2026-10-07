// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/PathGuard.hpp"

#include <array>
#include <system_error>

#include <sys/stat.h>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"

namespace akeno::security {

namespace fs = std::filesystem;

namespace {

// System partitions, device nodes, installed game content, game sandboxes and the
// configuration of other homebrew. Paths are listed in both their /data and /user/data
// spellings because /data is an alias of /user/data on the console.
constexpr std::array<std::string_view, 24> kForbiddenRoots{
    "/system",          "/system_ex",        "/system_data",       "/system_tmp",
    "/preinst",         "/preinst2",         "/update",            "/dev",
    "/boot",            "/mnt/sandbox",      "/mnt/shadowmnt",     "/user/app",
    "/user/appmeta",    "/user/patch",       "/user/addcont",      "/user/av_contents",
    "/user/home",       "/user/system",      "/user/license",      "/user/settings",
    "/data/shadowmount", "/user/data/shadowmount", "/data/etaHEN", "/user/data/etaHEN",
};

Error safety(std::string message, const fs::path& path) {
    return makeError(ErrorCode::SafetyViolation, std::move(message), path.string());
}

}  // namespace

Result<fs::path> normalizeAbsolute(std::string_view path) {
    if (path.empty() || path.front() != '/') {
        return makeError(ErrorCode::InvalidArgument, "Path must be absolute.", std::string(path));
    }
    if (path.find('\0') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "Path contains a NUL byte.");
    }
    if (path.size() > limits::kMaxPathBytes) {
        return makeError(ErrorCode::InvalidArgument, "Path is too long.",
                         strings::concat(path.size(), " bytes"));
    }
    std::string normalized;
    normalized.reserve(path.size());
    for (const std::string& component : strings::split(path, '/')) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return makeError(ErrorCode::InvalidArgument, "Path must not contain '..'.", std::string(path));
        }
        normalized.push_back('/');
        normalized.append(component);
    }
    if (normalized.empty()) {
        normalized = "/";
    }
    return fs::path(normalized);
}

bool isWithin(const fs::path& root, const fs::path& candidate) {
    const std::string& rootText = root.native();
    const std::string& candidateText = candidate.native();
    if (rootText == "/") {
        return !candidateText.empty() && candidateText.front() == '/';
    }
    if (candidateText.size() < rootText.size() || candidateText.compare(0, rootText.size(), rootText) != 0) {
        return false;
    }
    return candidateText.size() == rootText.size() || candidateText[rootText.size()] == '/';
}

std::span<const std::string_view> forbiddenRoots() { return kForbiddenRoots; }

bool isForbiddenPath(const fs::path& normalized) {
    if (normalized.native() == "/") {
        return true;  // the file-system root itself is never a valid write target
    }
    for (std::string_view root : kForbiddenRoots) {
        if (isWithin(fs::path(std::string(root)), normalized)) {
            return true;
        }
    }
    return false;
}

Result<WriteGuard> WriteGuard::create(const std::vector<fs::path>& allowedRoots) {
    if (allowedRoots.empty()) {
        return makeError(ErrorCode::InvalidArgument, "At least one writable root is required.");
    }
    std::vector<fs::path> roots;
    for (const fs::path& requested : allowedRoots) {
        auto normalized = normalizeAbsolute(requested.native());
        if (!normalized) {
            return std::move(normalized).error();
        }
        if (isForbiddenPath(normalized.value())) {
            return safety("A protected system location cannot be used for application data.",
                          normalized.value());
        }
        std::error_code ec;
        fs::path resolved = fs::weakly_canonical(normalized.value(), ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "Could not resolve the application data location.",
                             normalized.value().string() + ": " + ec.message());
        }
        auto resolvedNormalized = normalizeAbsolute(resolved.native());
        if (!resolvedNormalized) {
            return std::move(resolvedNormalized).error();
        }
        if (isForbiddenPath(resolvedNormalized.value())) {
            return safety("The application data location resolves into a protected system location.",
                          resolvedNormalized.value());
        }
        roots.push_back(std::move(resolvedNormalized).value());
    }
    return WriteGuard(std::move(roots));
}

Result<fs::path> WriteGuard::checkWritable(const fs::path& target) const {
    auto normalized = normalizeAbsolute(target.native());
    if (!normalized) {
        return std::move(normalized).error();
    }
    const fs::path& path = normalized.value();
    if (isForbiddenPath(path)) {
        return safety("Blocked a write to a protected system location.", path);
    }
    const fs::path* matchedRoot = nullptr;
    for (const fs::path& root : roots_) {
        if (isWithin(root, path)) {
            matchedRoot = &root;
            break;
        }
    }
    if (matchedRoot == nullptr) {
        return safety("Blocked a write outside the application's own storage.", path);
    }
    // Walk every component below the root. An existing symlink could redirect the write
    // elsewhere, so it is refused. Missing components end the walk: they will be created.
    std::string current = matchedRoot->native();
    std::string_view rest = std::string_view(path.native()).substr(current.size());
    for (const std::string& component : strings::split(rest, '/')) {
        if (component.empty()) {
            continue;
        }
        if (current.size() > 1) {
            current.push_back('/');
        }
        current.append(component);
        struct stat info {};
        if (::lstat(current.c_str(), &info) != 0) {
            break;
        }
        if (S_ISLNK(info.st_mode)) {
            return safety("Blocked a write through a symbolic link.", fs::path(current));
        }
    }
    return path;
}

}  // namespace akeno::security
