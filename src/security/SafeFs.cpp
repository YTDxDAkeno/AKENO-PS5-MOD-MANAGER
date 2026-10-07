// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/SafeFs.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "akeno/core/Strings.hpp"

namespace akeno::security {

namespace fs = std::filesystem;

namespace {

Error ioError(std::string message, const fs::path& path, int err) {
    ErrorCode code = ErrorCode::IoError;
    if (err == ENOSPC) code = ErrorCode::NoSpace;
    if (err == EACCES || err == EPERM || err == EROFS) code = ErrorCode::PermissionDenied;
    if (err == ENOENT) code = ErrorCode::NotFound;
    return makeError(code, std::move(message), path.string() + ": " + std::strerror(err));
}

Error ioError(std::string message, const fs::path& path, const std::error_code& ec) {
    return ioError(std::move(message), path, ec.value());
}

// Deletes a tree with plain path-based calls (lstat, opendir, unlink, rmdir). std::filesystem::
// remove_all was not used: on the first PS5 test it reported success and left the folder behind.
// Symlinks are removed, never followed. Returns 0 or the errno of the first failure.
int removeTreeAt(const std::string& path, int depth, std::string& failedPath) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) return 0;
        failedPath = path;
        return errno;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            failedPath = path;
            return errno;
        }
        return 0;
    }
    if (depth > 256) {
        failedPath = path;
        return ELOOP;
    }
    std::vector<std::string> children;
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        failedPath = path;
        return errno;
    }
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name == "." || name == "..") continue;
        children.emplace_back(path + "/" + std::string(name));
    }
    ::closedir(dir);
    for (const std::string& child : children) {
        if (int err = removeTreeAt(child, depth + 1, failedPath); err != 0) return err;
    }
    if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
        failedPath = path;
        return errno;
    }
    return 0;
}

// Flushes a directory entry change (create/rename) to disk. Best effort: some file systems
// do not allow fsync on directories.
void syncDirectory(const fs::path& directory) {
    int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

Status SafeFs::createDirectories(const fs::path& directory) const {
    auto checked = guard_.checkWritable(directory);
    if (!checked) {
        return std::move(checked).error();
    }
    std::error_code ec;
    fs::create_directories(checked.value(), ec);
    if (ec) {
        return ioError("Could not create a directory.", checked.value(), ec);
    }
    if (!fs::is_directory(checked.value(), ec)) {
        return makeError(ErrorCode::IoError, "Expected a directory but found something else.",
                         checked.value().string());
    }
    // Re-check after creation: nothing may have turned a component into a symlink.
    auto rechecked = guard_.checkWritable(checked.value());
    if (!rechecked) {
        return std::move(rechecked).error();
    }
    return {};
}

Status SafeFs::writeFileAtomic(const fs::path& target, std::string_view contents) const {
    auto checked = guard_.checkWritable(target);
    if (!checked) {
        return std::move(checked).error();
    }
    const fs::path& path = checked.value();
    fs::path temporary = path;
    temporary += strings::concat(".tmp-", ::getpid());
    auto tempChecked = guard_.checkWritable(temporary);
    if (!tempChecked) {
        return std::move(tempChecked).error();
    }

    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        return ioError("Could not create a file.", temporary, errno);
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            ::close(fd);
            ::unlink(temporary.c_str());
            return ioError("Could not write a file.", temporary, err);
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        int err = errno;
        ::close(fd);
        ::unlink(temporary.c_str());
        return ioError("Could not flush a file to disk.", temporary, err);
    }
    ::close(fd);
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        int err = errno;
        ::unlink(temporary.c_str());
        return ioError("Could not replace a file.", path, err);
    }
    syncDirectory(path.parent_path());
    return {};
}

WritableFile::~WritableFile() {
    if (fd_ >= 0) ::close(fd_);
}

Status WritableFile::write(std::string_view data) {
    if (fd_ < 0) {
        return makeError(ErrorCode::Internal, "The file is already closed.", path_.string());
    }
    std::size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd_, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ioError(errno == ENOSPC ? "The storage is full." : "Could not write a file.", path_, errno);
        }
        written += static_cast<std::size_t>(n);
    }
    size_ += data.size();
    return {};
}

Status WritableFile::sync() {
    if (fd_ >= 0 && ::fsync(fd_) != 0) {
        return ioError("Could not flush a file to disk.", path_, errno);
    }
    return {};
}

Status WritableFile::close(bool flush) {
    if (fd_ < 0) return {};
    Status synced = flush ? sync() : Status{};
    const int result = ::close(fd_);
    const int err = errno;
    fd_ = -1;
    if (!synced) return synced;
    if (result != 0) return ioError("Could not close a file.", path_, err);
    return {};
}

Result<std::unique_ptr<WritableFile>> SafeFs::openForWriting(const fs::path& target, WriteMode mode) const {
    auto checked = guard_.checkWritable(target);
    if (!checked) {
        return std::move(checked).error();
    }
    const fs::path& path = checked.value();
    int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC;
    switch (mode) {
        case WriteMode::Truncate: flags |= O_TRUNC; break;
        case WriteMode::Append: flags |= O_APPEND; break;
        case WriteMode::CreateNew: flags |= O_EXCL; break;
    }
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return makeError(ErrorCode::AlreadyExists, "The file already exists.", path.string());
        }
        return ioError("Could not open a file for writing.", path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        ::close(fd);
        return makeError(ErrorCode::SafetyViolation, "Refused to write to something that is not a plain file.",
                         path.string());
    }
    return std::unique_ptr<WritableFile>(new WritableFile(fd, path, static_cast<std::uint64_t>(info.st_size)));
}

Status SafeFs::removeFile(const fs::path& target) const {
    auto checked = guard_.checkWritable(target);
    if (!checked) {
        return std::move(checked).error();
    }
    if (::unlink(checked.value().c_str()) != 0 && errno != ENOENT) {
        return ioError("Could not remove a file.", checked.value(), errno);
    }
    return {};
}

Status SafeFs::removeTree(const fs::path& target) const {
    auto checked = guard_.checkWritable(target);
    if (!checked) {
        return std::move(checked).error();
    }
    for (const fs::path& root : guard_.roots()) {
        if (checked.value() == root) {
            return makeError(ErrorCode::SafetyViolation, "Refused to remove the application's storage root.",
                             root.string());
        }
    }
    std::string failedPath;
    if (int err = removeTreeAt(checked.value().string(), 0, failedPath); err != 0) {
        return ioError("Could not remove a directory.", failedPath, err);
    }
    // Trust only what is visible afterwards.
    struct stat info {};
    if (::lstat(checked.value().c_str(), &info) == 0) {
        return makeError(ErrorCode::IoError, "A folder was still there after it was deleted.",
                         checked.value().string());
    }
    return {};
}

Status SafeFs::rename(const fs::path& from, const fs::path& to) const {
    auto checkedFrom = guard_.checkWritable(from);
    if (!checkedFrom) {
        return std::move(checkedFrom).error();
    }
    auto checkedTo = guard_.checkWritable(to);
    if (!checkedTo) {
        return std::move(checkedTo).error();
    }
    if (::rename(checkedFrom.value().c_str(), checkedTo.value().c_str()) != 0) {
        return ioError("Could not rename.", checkedFrom.value(), errno);
    }
    syncDirectory(checkedTo.value().parent_path());
    if (checkedFrom.value().parent_path() != checkedTo.value().parent_path()) {
        syncDirectory(checkedFrom.value().parent_path());
    }
    return {};
}

Status SafeFs::copyFile(const fs::path& from, const fs::path& to) const {
    auto checkedTo = guard_.checkWritable(to);
    if (!checkedTo) {
        return std::move(checkedTo).error();
    }
    std::error_code ec;
    fs::copy_file(from, checkedTo.value(), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return ioError("Could not copy a file.", checkedTo.value(), ec);
    }
    return {};
}

Status SafeFs::createHardLink(const fs::path& existing, const fs::path& target) const {
    auto checkedExisting = guard_.checkWritable(existing);
    if (!checkedExisting) {
        return std::move(checkedExisting).error();
    }
    auto checkedTarget = guard_.checkWritable(target);
    if (!checkedTarget) {
        return std::move(checkedTarget).error();
    }
    if (::link(checkedExisting.value().c_str(), checkedTarget.value().c_str()) != 0) {
        return ioError("Could not create a hard link.", checkedTarget.value(), errno);
    }
    return {};
}

Result<std::string> readFileBounded(const fs::path& path, std::size_t maxBytes) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return ioError("Could not open a file.", path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        int err = errno;
        ::close(fd);
        return ioError("Could not inspect a file.", path, err);
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        return makeError(ErrorCode::InvalidArgument, "Not a regular file.", path.string());
    }
    if (static_cast<std::uint64_t>(info.st_size) > maxBytes) {
        ::close(fd);
        return makeError(ErrorCode::ResponseTooLarge, "The file is larger than allowed.",
                         strings::concat(path.string(), ": ", info.st_size, " > ", maxBytes));
    }
    std::string contents;
    contents.resize(static_cast<std::size_t>(info.st_size));
    std::size_t total = 0;
    while (total < contents.size()) {
        ssize_t n = ::read(fd, contents.data() + total, contents.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            ::close(fd);
            return ioError("Could not read a file.", path, err);
        }
        if (n == 0) break;  // file shrank while reading
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);
    contents.resize(total);
    return contents;
}

Result<StorageSpace> queryStorageSpace(const fs::path& path) {
    struct statvfs info {};
    if (::statvfs(path.c_str(), &info) != 0) {
        return ioError("Could not read free space.", path, errno);
    }
    StorageSpace space;
    space.totalBytes = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
    space.availableBytes = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
    return space;
}

}  // namespace akeno::security
