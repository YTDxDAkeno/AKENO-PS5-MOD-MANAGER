// SPDX-License-Identifier: GPL-3.0-or-later
// All file-system mutations performed by Akeno go through SafeFs, which consults the
// WriteGuard before touching anything.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "akeno/core/Result.hpp"
#include "akeno/security/PathGuard.hpp"

namespace akeno::security {

class SafeFs {
public:
    explicit SafeFs(WriteGuard guard) : guard_(std::move(guard)) {}

    const WriteGuard& guard() const noexcept { return guard_; }

    Status createDirectories(const std::filesystem::path& directory) const;

    // Writes to "<target>.tmp-<pid>", flushes it to disk, then renames over `target`.
    // A crash leaves either the old or the new file, never a partial one.
    Status writeFileAtomic(const std::filesystem::path& target, std::string_view contents) const;

    Status removeFile(const std::filesystem::path& target) const;

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
