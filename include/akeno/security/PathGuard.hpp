// SPDX-License-Identifier: GPL-3.0-or-later
// Write-location enforcement (safety model rules H1 and H2).
#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "akeno/core/Result.hpp"

namespace akeno::security {

// Lexically normalizes an absolute POSIX path. Collapses repeated separators and "."
// components and strips a trailing separator. Rejects relative paths, embedded NUL bytes,
// ".." components, and paths longer than limits::kMaxPathBytes. The file system is not
// consulted.
Result<std::filesystem::path> normalizeAbsolute(std::string_view path);

// True if `candidate` is `root` or lies below it. Both must already be normalized.
bool isWithin(const std::filesystem::path& root, const std::filesystem::path& candidate);

// Locations that Akeno must never write to, whatever the configuration says.
std::span<const std::string_view> forbiddenRoots();
bool isForbiddenPath(const std::filesystem::path& normalized);

// Gate for every file-system mutation (create, write, rename, remove).
class WriteGuard {
public:
    // Every root must be absolute and not forbidden. Existing symlinks in a root are resolved
    // once here, so later checks compare against the real location.
    static Result<WriteGuard> create(const std::vector<std::filesystem::path>& allowedRoots);

    // Returns the normalized path if `target` may be mutated. Requirements: absolute and
    // normalized, not on the deny-list, inside an allowed root, and no symlink in any existing
    // component between the root and the target.
    Result<std::filesystem::path> checkWritable(const std::filesystem::path& target) const;

    const std::vector<std::filesystem::path>& roots() const noexcept { return roots_; }

private:
    explicit WriteGuard(std::vector<std::filesystem::path> roots) : roots_(std::move(roots)) {}
    std::vector<std::filesystem::path> roots_;
};

}  // namespace akeno::security
