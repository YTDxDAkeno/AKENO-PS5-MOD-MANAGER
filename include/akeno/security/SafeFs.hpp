// SPDX-License-Identifier: GPL-3.0-or-later
// All file-system mutations performed by Akeno go through SafeFs, which consults the
// WriteGuard before touching anything.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "akeno/core/Result.hpp"
#include "akeno/security/PathGuard.hpp"

namespace akeno::security {

enum class WriteMode {
    Truncate,   // create or empty an existing file
    Append,     // create or continue an existing file
    CreateNew,  // the file must not exist yet
};

// A file opened for writing through SafeFs (downloads, extraction). Closed on destruction.
class WritableFile {
public:
    ~WritableFile();
    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;

    Status write(std::string_view data);
    Status sync();    // fsync
    // Closes the file; with `flush` (the default) it is synced first. Further writes fail.
    Status close(bool flush = true);
    std::uint64_t size() const noexcept { return size_; }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    friend class SafeFs;
    WritableFile(int fd, std::filesystem::path path, std::uint64_t size) : fd_(fd), path_(std::move(path)), size_(size) {}
    int fd_ = -1;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
};

class SafeFs {
public:
    explicit SafeFs(WriteGuard guard) : guard_(std::move(guard)) {}

    const WriteGuard& guard() const noexcept { return guard_; }

    Status createDirectories(const std::filesystem::path& directory) const;

    // Writes to "<target>.tmp-<pid>", flushes it to disk, then renames over `target`.
    // A crash leaves either the old or the new file, never a partial one.
    Status writeFileAtomic(const std::filesystem::path& target, std::string_view contents) const;

    Status removeFile(const std::filesystem::path& target) const;

    // Opens a regular file for writing without following symlinks; never a hard-linked file.
    Result<std::unique_ptr<WritableFile>> openForWriting(const std::filesystem::path& target, WriteMode mode) const;

    // Removes a directory tree without following symlinks. Refuses to remove an allowed root.
    Status removeTree(const std::filesystem::path& target) const;

    Status rename(const std::filesystem::path& from, const std::filesystem::path& to) const;

    Status copyFile(const std::filesystem::path& from, const std::filesystem::path& to) const;

    // Creates `target` as a hard link to `existing`. Both must be inside allowed roots.
    Status createHardLink(const std::filesystem::path& existing, const std::filesystem::path& target) const;

private:
    WriteGuard guard_;
};

// Reading is not restricted by the guard, but is always bounded.
Result<std::string> readFileBounded(const std::filesystem::path& path, std::size_t maxBytes);

struct StorageSpace {
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
};
Result<StorageSpace> queryStorageSpace(const std::filesystem::path& path);

}  // namespace akeno::security
