// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/SafeOpen.hpp"

#include <cerrno>
#include <cstring>

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
    const int copy = ::fcntl(directoryFd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return UniqueFd();
    return walk(UniqueFd(copy), relative, directory);
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
