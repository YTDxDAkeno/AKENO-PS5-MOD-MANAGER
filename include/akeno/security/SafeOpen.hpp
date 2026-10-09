// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only opening of files and folders that never follows a symbolic link, also not in an
// ancestor: every component is opened with openat(O_NOFOLLOW) from the previous descriptor,
// so a path cannot be redirected between checking and opening it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace akeno::security {

// Owns one file descriptor.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd();
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;
};

// Opens an absolute, normalised path read-only. Returns an invalid descriptor and sets errno on
// failure (ELOOP/ENOTDIR for a link in the path, EINVAL for a path that is not normalised).
UniqueFd openNoFollow(const std::filesystem::path& absolute, bool directory);

// Opens `relative` ('/'-separated, validated) below an already opened folder, read-only and
// without following links.
UniqueFd openBelow(int directoryFd, std::string_view relative, bool directory);

// A second descriptor for the same open file (for fdopendir(), which takes ownership, or a
// reader that closes its own copy). Uses dup(): fcntl(F_DUPFD_CLOEXEC) was refused on a PS5
// (firmware 12.20: no file below an opened folder could be read), dup() is what the
// hardware-tested 0.2.0-alpha used. Close-on-exec is set when the kernel accepts it; Akeno never
// executes anything, so it is not required. Returns -1 with errno set on failure.
int duplicateDescriptor(int fd);

// Opens a probe file below `folder` the way mod checks and game listings do (open the folder,
// duplicate its descriptor, open below it, list it). Empty when it works, else the failing step
// and errno. Used by the system check so a console report shows whether this works.
std::string probeFolderAccess(const std::filesystem::path& folder, std::string_view probeFile);

// "ENOENT (No such file or directory)": errno values are kept in reports so that behaviour seen
// on a console can be told apart from Akeno's own decisions.
std::string describeErrno(int error);

}  // namespace akeno::security
