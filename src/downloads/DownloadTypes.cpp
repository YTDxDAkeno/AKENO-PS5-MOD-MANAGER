// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/downloads/DownloadTypes.hpp"

#include <charconv>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::downloads {

std::string_view toString(DownloadState state) noexcept {
    switch (state) {
        case DownloadState::Queued: return "queued";
        case DownloadState::Downloading: return "downloading";
        case DownloadState::Paused: return "paused";
        case DownloadState::Verifying: return "verifying";
        case DownloadState::Completed: return "completed";
        case DownloadState::Failed: return "failed";
    }
    return "failed";
}

std::optional<DownloadState> parseDownloadState(std::string_view text) noexcept {
    for (DownloadState state : {DownloadState::Queued, DownloadState::Downloading, DownloadState::Paused,
                                DownloadState::Verifying, DownloadState::Completed, DownloadState::Failed}) {
        if (toString(state) == text) return state;
    }
    return std::nullopt;
}

bool isValidDownloadId(std::string_view id) noexcept {
    if (id.size() != 16) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string_view fileExtension(mods::ArchiveFormat format) noexcept {
    switch (format) {
        case mods::ArchiveFormat::Zip: return "zip";
        case mods::ArchiveFormat::Tar: return "tar";
        case mods::ArchiveFormat::TarGz: return "tar.gz";
        case mods::ArchiveFormat::SevenZip: return "7z";
        case mods::ArchiveFormat::Unknown: return {};
    }
    return {};
}

std::filesystem::path partialPath(const std::filesystem::path& directory, std::string_view id) {
    return directory / (std::string(id) + ".partial");
}

std::filesystem::path finalPath(const std::filesystem::path& directory, std::string_view id,
                                mods::ArchiveFormat format) {
    return directory / (std::string(id) + "." + std::string(fileExtension(format)));
}

bool isDownloadFileName(std::string_view name, std::string* id) noexcept {
    if (name.size() < 18 || name[16] != '.') return false;
    const std::string_view candidate = name.substr(0, 16);
    if (!isValidDownloadId(candidate)) return false;
    const std::string_view suffix = name.substr(17);
    const bool known = suffix == "partial" || suffix == "zip" || suffix == "tar" || suffix == "tar.gz" || suffix == "7z";
    if (known && id != nullptr) *id = std::string(candidate);
    return known;
}

Status validateRequest(const DownloadRequest& request) {
    if (!mods::isAllowedRemoteUrl(request.url) || request.url.size() > 2048) {
        return makeError(ErrorCode::SafetyViolation, "The download address is not allowed (https only).",
                         request.url);
    }
    if (request.expectedSize == 0 || request.expectedSize > mods::kMaxDownloadSize) {
        return makeError(ErrorCode::InvalidArgument, "The download size is unknown or too large.",
                         std::to_string(request.expectedSize));
    }
    if (!security::isSha256Hex(request.expectedSha256)) {
        return makeError(ErrorCode::InvalidArgument,
                         "The download has no valid SHA-256 checksum, so it could not be verified.");
    }
    if (fileExtension(request.format).empty()) {
        return makeError(ErrorCode::Unsupported, "The archive format of this mod is not supported.");
    }
    if (request.mod.providerId.empty() || request.mod.modId.empty() || request.mod.modId.size() > 255) {
        return makeError(ErrorCode::InvalidArgument, "The download does not name its mod.");
    }
    if (!mods::isSafeRelativePath(request.archiveRoot) || !mods::isSafeRelativePath(request.targetPrefix)) {
        return makeError(ErrorCode::SafetyViolation, "The mod's install folders are not safe paths.");
    }
    if (request.displayName.empty() || request.displayName.size() > limits::kMaxDisplayStringBytes) {
        return makeError(ErrorCode::InvalidArgument, "The download has no valid name.");
    }
    return {};
}

std::optional<ContentRange> parseContentRange(std::string_view value) noexcept {
    value = strings::trim(value);
    constexpr std::string_view kPrefix = "bytes ";
    if (value.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    value.remove_prefix(kPrefix.size());
    const std::size_t dash = value.find('-');
    const std::size_t slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash > slash) return std::nullopt;
    auto number = [](std::string_view text, std::uint64_t& out) {
        if (text.empty()) return false;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && ptr == text.data() + text.size();
    };
    ContentRange range;
    if (!number(value.substr(0, dash), range.first) || !number(value.substr(dash + 1, slash - dash - 1), range.last)) {
        return std::nullopt;
    }
    const std::string_view total = value.substr(slash + 1);
    if (total != "*") {
        std::uint64_t parsed = 0;
        if (!number(total, parsed)) return std::nullopt;
        range.total = parsed;
        if (range.last >= parsed) return std::nullopt;
    }
    if (range.last < range.first) return std::nullopt;
    return range;
}

}  // namespace akeno::downloads
