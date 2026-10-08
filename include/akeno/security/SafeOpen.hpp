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

// "ENOENT (No such file or directory)": errno values are kept in reports so that behaviour seen
// on a console can be told apart from Akeno's own decisions.
std::string describeErrno(int error);

}  // namespace akeno::security
