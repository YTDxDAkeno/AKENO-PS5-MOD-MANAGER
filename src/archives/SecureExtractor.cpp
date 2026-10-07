// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/archives/SecureExtractor.hpp"

#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akeno/core/Strings.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::archives {

namespace fs = std::filesystem;
using logging::logger;

namespace {

// Length of the UTF-8 sequence starting at text[pos], or 0 if it is not valid (overlong
// encodings, surrogates and values above U+10FFFF are invalid).
std::size_t utf8Length(std::string_view text, std::size_t pos) {
    const auto c = static_cast<unsigned char>(text[pos]);
    std::size_t length = 0;
    std::uint32_t value = 0;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) {
        length = 2;
        value = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3;
        value = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4;
        value = c & 0x07;
    } else {
        return 0;
    }
    if (pos + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[pos + i]);
        if ((next & 0xC0) != 0x80) return 0;
        value = (value << 6) | (next & 0x3F);
    }
    if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
        value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return 0;
    }
    return length;
}

Error refused(std::string why, std::string_view name) {
    return makeError(ErrorCode::SafetyViolation, "The archive was refused: " + why + ".",
                     strings::sanitizeForDisplay(name, 300));
}

struct ArchiveDeleter {
    void operator()(struct archive* a) const { archive_read_free(a); }
};
using ArchivePtr = std::unique_ptr<struct archive, ArchiveDeleter>;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) ::close(fd_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

const char* formatName(mods::ArchiveFormat format) {
    switch (format) {
        case mods::ArchiveFormat::Zip: return "zip";
        case mods::ArchiveFormat::Tar: return "tar";
        case mods::ArchiveFormat::TarGz: return "tar.gz";
        case mods::ArchiveFormat::SevenZip: return "7z";
        case mods::ArchiveFormat::Unknown: return "unknown";
    }
    return "unknown";
}

// Opens the archive with only the reader for its declared format (smaller attack surface, and a
// file that is something else is refused).
Result<ArchivePtr> openArchive(const FileDescriptor& fd, mods::ArchiveFormat format) {
    ArchivePtr a(archive_read_new());
    if (!a) return makeError(ErrorCode::Internal, "Could not prepare the archive reader.");
    switch (format) {
        case mods::ArchiveFormat::Zip: archive_read_support_format_zip(a.get()); break;
        case mods::ArchiveFormat::Tar: archive_read_support_format_tar(a.get()); break;
        case mods::ArchiveFormat::TarGz:
            archive_read_support_format_tar(a.get());
            archive_read_support_filter_gzip(a.get());
            break;
        case mods::ArchiveFormat::SevenZip: archive_read_support_format_7zip(a.get()); break;
        case mods::ArchiveFormat::Unknown:
            return makeError(ErrorCode::Unsupported, "The archive format is not supported.");
    }
    if (archive_read_open_fd(a.get(), fd.get(), 64 * 1024) != ARCHIVE_OK) {
        return makeError(ErrorCode::ParseError,
                         strings::concat("The file is not a valid ", formatName(format), " archive."),
                         archive_error_string(a.get()) != nullptr ? archive_error_string(a.get()) : "");
    }
    return a;
}

Result<std::unique_ptr<FileDescriptor>> openReadOnly(const fs::path& path, std::uint64_t& size) {
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return makeError(ErrorCode::NotFound, "The archive could not be opened.", path.string());
    }
    auto holder = std::make_unique<FileDescriptor>(fd);
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        return makeError(ErrorCode::SafetyViolation, "The archive is not a plain file.", path.string());
    }
    size = static_cast<std::uint64_t>(info.st_size);
    return holder;
}

// Checks one header. Returns the normalised entry or the reason for refusing the archive.
Result<ArchiveEntry> checkEntry(struct archive_entry* entry, const ExtractionLimits& limits) {
    const char* utf8 = archive_entry_pathname_utf8(entry);
    const char* raw = archive_entry_pathname(entry);
    if (utf8 == nullptr) {
        return refused("an entry name is not valid text", raw != nullptr ? raw : "");
    }
    if (archive_entry_is_encrypted(entry)) {
        return refused("it is password protected, which is not supported", utf8);
    }
    if (archive_entry_hardlink(entry) != nullptr) {
        return refused("it contains a hard link", utf8);
    }
    ArchiveEntry result;
    switch (archive_entry_filetype(entry)) {
        case AE_IFREG: result.type = EntryType::File; break;
        case AE_IFDIR: result.type = EntryType::Directory; break;
        case AE_IFLNK: return refused("it contains a symbolic link", utf8);
        default: return refused("it contains a device, pipe or socket", utf8);
    }
    auto normalized = normalizeEntryPath(utf8, limits);
    if (!normalized) return std::move(normalized).error();
    result.path = std::move(normalized).value();
    if (result.type == EntryType::File) {
        const la_int64_t size = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : 0;
        if (size < 0) return refused("an entry has an invalid size", utf8);
        result.size = static_cast<std::uint64_t>(size);
        if (result.size > limits.maxEntryBytes) {
            return refused(strings::concat("a file is larger than ", strings::formatBytes(limits.maxEntryBytes)), utf8);
        }
    }
    return result;
}

}  // namespace

Result<std::string> normalizeEntryPath(std::string_view name, const ExtractionLimits& limits) {
    if (name.empty()) return refused("an entry has no name", name);
    if (name.size() > limits.maxPathBytes) return refused("an entry name is too long", name);
    for (std::size_t pos = 0; pos < name.size();) {
        const auto c = static_cast<unsigned char>(name[pos]);
        if (c < 0x20 || c == 0x7F) return refused("an entry name contains control characters", name);
        if (c == '\\') return refused("an entry name contains a backslash", name);
        if (c == ':') return refused("an entry name contains a colon or drive letter", name);
        const std::size_t length = utf8Length(name, pos);
        if (length == 0) return refused("an entry name is not valid UTF-8", name);
        pos += length;
    }
    if (name.front() == '/') return refused("an entry has an absolute path", name);
    std::string_view rest = name;
    if (rest.back() == '/') rest.remove_suffix(1);  // directory entries end with a slash
    if (rest.empty()) return refused("an entry has no name", name);
    std::string normalized;
    int depth = 0;
    std::size_t start = 0;
    while (start <= rest.size()) {
        std::size_t end = rest.find('/', start);
        if (end == std::string_view::npos) end = rest.size();
        const std::string_view component = rest.substr(start, end - start);
        if (component.empty()) return refused("an entry name contains an empty folder name", name);
        if (component == "..") return refused("an entry tries to leave its folder (\"..\")", name);
        if (component == ".") return refused("an entry name contains \".\"", name);
        if (component.size() > limits.maxComponentBytes) return refused("a file or folder name is too long", name);
        if (++depth > limits.maxDepth) return refused("the folders are nested too deeply", name);
        if (!normalized.empty()) normalized.push_back('/');
        normalized.append(component);
        if (end == rest.size()) break;
        start = end + 1;
    }
    return normalized;
}

Result<ArchiveListing> SecureExtractor::inspect(const fs::path& archive, mods::ArchiveFormat format,
                                                const CancellationToken* cancel) const {
    std::uint64_t archiveBytes = 0;
    auto fd = openReadOnly(archive, archiveBytes);
    if (!fd) return std::move(fd).error();
    auto opened = openArchive(*fd.value(), format);
    if (!opened) return std::move(opened).error();
    struct archive* a = opened->get();

    ArchiveListing listing;
    listing.archiveBytes = archiveBytes;
    std::set<std::string> files;
    std::set<std::string> directories;
    struct archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        if (cancel != nullptr && cancel->cancelled()) {
            return makeError(ErrorCode::Cancelled, "The check was cancelled.");
        }
        if (status == ARCHIVE_WARN && archive_error_string(a) != nullptr) {
            logger().debug("archives", std::string("libarchive warning: ") + archive_error_string(a));
        }
        auto checked = checkEntry(entry, limits_);
        if (!checked) return std::move(checked).error();
        ArchiveEntry& item = checked.value();
        if (listing.entries.size() >= limits_.maxEntries) {
            return refused(strings::concat("it has more than ", limits_.maxEntries, " entries"), item.path);
        }
        if (item.type == EntryType::File) {
            if (!files.insert(item.path).second || directories.count(item.path) != 0) {
                return refused("it contains the same name twice", item.path);
            }
            ++listing.fileCount;
            listing.totalBytes += item.size;
            if (listing.totalBytes > limits_.maxTotalBytes) {
                return refused(strings::concat("it would unpack to more than ", strings::formatBytes(limits_.maxTotalBytes)),
                               item.path);
            }
        } else if (files.count(item.path) != 0) {
            return refused("it contains the same name twice", item.path);
        } else {
            directories.insert(item.path);
        }
        listing.entries.push_back(std::move(item));
        if (archive_read_data_skip(a) != ARCHIVE_OK) {
            return makeError(ErrorCode::ParseError, "The archive is damaged.",
                             archive_error_string(a) != nullptr ? archive_error_string(a) : "");
        }
    }
    if (status != ARCHIVE_EOF) {
        return makeError(ErrorCode::ParseError, "The archive is damaged or not supported.",
                         archive_error_string(a) != nullptr ? archive_error_string(a) : "");
    }
    // A file may not also be used as a folder ("a" and "a/b").
    for (const auto& path : files) {
        for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
            if (files.count(path.substr(0, slash)) != 0) return refused("a file name is also used as a folder", path);
        }
    }
    if (listing.totalBytes > limits_.ratioCheckFrom && archiveBytes > 0 &&
        static_cast<double>(listing.totalBytes) / static_cast<double>(archiveBytes) > limits_.maxCompressionRatio) {
        return refused("it is compressed unusually strongly (a possible archive bomb)", archive.filename().string());
    }
    if (const char* name = archive_format_name(a)) listing.formatName = name;
    return listing;
}

Result<ExtractedTree> SecureExtractor::extract(const fs::path& archive, mods::ArchiveFormat format,
                                               const fs::path& destination, const CancellationToken* cancel,
                                               const std::function<void(std::uint64_t, std::uint64_t)>& progress) const {
    auto listing = inspect(archive, format, cancel);
    if (!listing) return std::move(listing).error();

    std::error_code ec;
    if (fs::exists(fs::symlink_status(destination, ec))) {
        return makeError(ErrorCode::AlreadyExists, "The staging folder already exists.", destination.string());
    }
    AKENO_TRY(fs_.createDirectories(destination));
    auto tree = extractInto(archive, format, destination, listing.value(), cancel, progress);
    if (tree) {
        auto verified = verifyTree(tree.value());
        if (verified) return tree;
        tree = std::move(verified).error();
    }
    auto removed = fs_.removeTree(destination);
    if (!removed) {
        logger().error("archives", "could not remove the staging folder: " + removed.error().describe());
    }
    return tree;
}

Result<ExtractedTree> SecureExtractor::extractInto(const fs::path& archive, mods::ArchiveFormat format,
                                                   const fs::path& destination, const ArchiveListing& listing,
                                                   const CancellationToken* cancel,
                                                   const std::function<void(std::uint64_t, std::uint64_t)>& progress) const {
    std::uint64_t archiveBytes = 0;
    auto fd = openReadOnly(archive, archiveBytes);
    if (!fd) return std::move(fd).error();
    if (archiveBytes != listing.archiveBytes) {
        return makeError(ErrorCode::SafetyViolation, "The archive changed while it was being checked.");
    }
    auto opened = openArchive(*fd.value(), format);
    if (!opened) return std::move(opened).error();
    struct archive* a = opened->get();

    ExtractedTree tree;
    tree.root = destination;
    tree.listing = listing;
    std::vector<char> buffer(256 * 1024);
    std::uint64_t written = 0;
    std::size_t index = 0;
    struct archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        if (cancel != nullptr && cancel->cancelled()) {
            return makeError(ErrorCode::Cancelled, "The check was cancelled.");
        }
        auto checked = checkEntry(entry, limits_);
        if (!checked) return std::move(checked).error();
        // The second pass must see exactly what the first one checked.
        if (index >= listing.entries.size() || listing.entries[index].path != checked->path ||
            listing.entries[index].type != checked->type) {
            return makeError(ErrorCode::SafetyViolation, "The archive changed while it was being read.");
        }
        const ArchiveEntry& item = listing.entries[index++];
        const fs::path target = destination / fs::path(item.path);
        if (item.type == EntryType::Directory) {
            AKENO_TRY(fs_.createDirectories(target));
            continue;
        }
        AKENO_TRY(fs_.createDirectories(target.parent_path()));
        auto file = fs_.openForWriting(target, security::WriteMode::CreateNew);
        if (!file) return std::move(file).error();
        security::Sha256 hash;
        ExtractedFile extracted;
        extracted.path = item.path;
        while (true) {
            const la_ssize_t n = archive_read_data(a, buffer.data(), buffer.size());
            if (n < 0) {
                return makeError(ErrorCode::ParseError, "The archive is damaged.",
                                 archive_error_string(a) != nullptr ? archive_error_string(a) : "");
            }
            if (n == 0) break;
            const auto count = static_cast<std::size_t>(n);
            extracted.size += count;
            written += count;
            // Never trust declared sizes: count what really comes out.
            if (extracted.size > item.size || extracted.size > limits_.maxEntryBytes || written > limits_.maxTotalBytes) {
                return refused("a file contains more data than its header says", item.path);
            }
            const std::string_view chunk(buffer.data(), count);
            if (extracted.head.size() < kHeadBytes) {
                extracted.head.append(chunk.substr(0, kHeadBytes - extracted.head.size()));
            }
            hash.update(chunk);
            AKENO_TRY(file.value()->write(chunk));
            if (cancel != nullptr && cancel->cancelled()) {
                return makeError(ErrorCode::Cancelled, "The check was cancelled.");
            }
            if (progress) progress(written, listing.totalBytes);
        }
        if (extracted.size != item.size) {
            return refused("a file is shorter than its header says", item.path);
        }
        AKENO_TRY(file.value()->close(false));  // staging is temporary; no fsync per file
        extracted.sha256 = hash.finishHex();
        tree.files.push_back(std::move(extracted));
    }
    if (status != ARCHIVE_EOF || index != listing.entries.size()) {
        return makeError(ErrorCode::ParseError, "The archive is damaged or not supported.",
                         archive_error_string(a) != nullptr ? archive_error_string(a) : "");
    }
    return tree;
}

Status SecureExtractor::verifyTree(const ExtractedTree& tree) const {
    // Re-scan what is on disk without following anything: only plain files and folders that
    // the archive listed may exist below the staging folder.
    std::set<std::string> expectedFiles;
    std::set<std::string> expectedDirectories;
    for (const auto& file : tree.files) {
        expectedFiles.insert(file.path);
        for (std::size_t slash = file.path.find('/'); slash != std::string::npos; slash = file.path.find('/', slash + 1)) {
            expectedDirectories.insert(file.path.substr(0, slash));
        }
    }
    for (const auto& entry : tree.listing.entries) {
        if (entry.type == EntryType::Directory) {
            expectedDirectories.insert(entry.path);
            for (std::size_t slash = entry.path.find('/'); slash != std::string::npos;
                 slash = entry.path.find('/', slash + 1)) {
                expectedDirectories.insert(entry.path.substr(0, slash));
            }
        }
    }
    std::error_code ec;
    std::size_t seenFiles = 0;
    for (fs::recursive_directory_iterator it(tree.root, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        const std::string relative = it->path().lexically_relative(tree.root).generic_string();
        if (fs::is_regular_file(status) && expectedFiles.count(relative) != 0) {
            ++seenFiles;
        } else if (!(fs::is_directory(status) && expectedDirectories.count(relative) != 0)) {
            return makeError(ErrorCode::SafetyViolation, "Unexpected content appeared in the staging folder.", relative);
        }
    }
    if (ec) {
        return makeError(ErrorCode::IoError, "Could not check the staging folder.", ec.message());
    }
    if (seenFiles != expectedFiles.size()) {
        return makeError(ErrorCode::SafetyViolation, "Files are missing from the staging folder.");
    }
    return {};
}

}  // namespace akeno::archives
