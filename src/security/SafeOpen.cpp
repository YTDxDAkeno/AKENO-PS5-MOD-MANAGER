// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/SafeOpen.hpp"

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "akeno/core/Strings.hpp"
#include "akeno/security/PathGuard.hpp"

namespace akeno::security {

UniqueFd::~UniqueFd() {
    if (fd_ >= 0) ::close(fd_);
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.release();
    }
    return *this;
}

namespace {

UniqueFd walk(UniqueFd current, std::string_view relative, bool directory) {
    std::size_t start = 0;
    while (start < relative.size()) {
        std::size_t end = relative.find('/', start);
        if (end == std::string_view::npos) end = relative.size();
        const std::string part(relative.substr(start, end - start));
        start = end + 1;
        if (part.empty() || part == "." || part == "..") {
            errno = EINVAL;
            return UniqueFd();
        }
        const bool last = start >= relative.size();
        const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | ((directory || !last) ? O_DIRECTORY : 0);
        const int child = ::openat(current.get(), part.c_str(), flags);
        if (child < 0) return UniqueFd();
        current = UniqueFd(child);
    }
    return current;
}

}  // namespace

UniqueFd openNoFollow(const std::filesystem::path& absolute, bool directory) {
    auto normalized = normalizeAbsolute(absolute.string());
    if (!normalized || normalized->string() != absolute.string()) {
        errno = EINVAL;
        return UniqueFd();
    }
    UniqueFd root(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!root.valid()) return root;
    const std::string relative = normalized->relative_path().string();
    if (relative.empty()) return root;
    return walk(std::move(root), relative, directory);
}

UniqueFd openBelow(int directoryFd, std::string_view relative, bool directory) {
    if (directoryFd < 0 || relative.empty() || relative.front() == '/') {
        errno = EINVAL;
        return UniqueFd();
    }
    const int copy = duplicateDescriptor(directoryFd);
    if (copy < 0) return UniqueFd();
    return walk(UniqueFd(copy), relative, directory);
}

int duplicateDescriptor(int fd) {
    const int copy = ::dup(fd);
    if (copy < 0) return -1;
    const int error = errno;
    (void)::fcntl(copy, F_SETFD, FD_CLOEXEC);  // best effort
    errno = error;
    return copy;
}

std::string probeFolderAccess(const std::filesystem::path& folder, std::string_view probeFile) {
    UniqueFd root = openNoFollow(folder, true);
    if (!root.valid()) return "opening " + folder.string() + ": " + describeErrno(errno);
    UniqueFd file = openBelow(root.get(), probeFile, false);
    if (!file.valid()) return "opening " + std::string(probeFile) + " below it: " + describeErrno(errno);
    char byte = 0;
    if (::pread(file.get(), &byte, 1, 0) < 0) return "reading " + std::string(probeFile) + ": " + describeErrno(errno);
    const int copy = duplicateDescriptor(root.get());
    if (copy < 0) return "duplicating the folder descriptor: " + describeErrno(errno);
    DIR* dir = ::fdopendir(copy);
    if (dir == nullptr) {
        const int error = errno;
        ::close(copy);
        return "listing the folder: " + describeErrno(error);
    }
    bool found = false;
    const std::string name(probeFile.substr(probeFile.rfind('/') == std::string_view::npos ? 0 : probeFile.rfind('/') + 1));
    while (const dirent* entry = ::readdir(dir)) {
        if (name == entry->d_name) found = true;
    }
    ::closedir(dir);
    return found || probeFile.find('/') != std::string_view::npos ? std::string() : "the probe file is not listed";
}

std::string describeErrno(int error) {
    const char* name = nullptr;
    switch (error) {
        case ENOENT: name = "ENOENT"; break;
        case EACCES: name = "EACCES"; break;
        case EPERM: name = "EPERM"; break;
        case ENOTDIR: name = "ENOTDIR"; break;
        case ELOOP: name = "ELOOP"; break;
        case EBADF: name = "EBADF"; break;
        case EINVAL: name = "EINVAL"; break;
        case EIO: name = "EIO"; break;
        case EMFILE: name = "EMFILE"; break;
        case ENFILE: name = "ENFILE"; break;
        case ENAMETOOLONG: name = "ENAMETOOLONG"; break;
        case EBUSY: name = "EBUSY"; break;
        case EAGAIN: name = "EAGAIN"; break;
        case EXDEV: name = "EXDEV"; break;
        case E2BIG: name = "E2BIG"; break;
        default: break;
    }
    const std::string text = std::strerror(error);
    return name != nullptr ? strings::concat(name, " (", text, ")") : strings::concat("errno ", error, " (", text, ")");
}

}  // namespace akeno::security
