// SPDX-License-Identifier: GPL-3.0-or-later
// Writes test archives with libarchive's writer, including hostile ones.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "akeno/mods/Catalog.hpp"

namespace akeno::test {

struct EntrySpec {
    std::string name;
    std::string data;
    unsigned type = AE_IFREG;
    std::string symlink;
    std::string hardlink;
};

// Returns false if this libarchive cannot write the requested variant.
inline bool writeArchive(const std::filesystem::path& path, mods::ArchiveFormat format, const std::vector<EntrySpec>& entries,
                  bool encrypt = false) {
    struct archive* a = archive_write_new();
    switch (format) {
        case mods::ArchiveFormat::Zip: archive_write_set_format_zip(a); break;
        case mods::ArchiveFormat::Tar: archive_write_set_format_pax_restricted(a); break;
        case mods::ArchiveFormat::TarGz:
            archive_write_set_format_pax_restricted(a);
            archive_write_add_filter_gzip(a);
            break;
        case mods::ArchiveFormat::SevenZip: archive_write_set_format_7zip(a); break;
        case mods::ArchiveFormat::Unknown: archive_write_free(a); return false;
    }
    if (encrypt && (archive_write_set_options(a, "zip:encryption=zipcrypt") != ARCHIVE_OK ||
                    archive_write_set_passphrase(a, "secret") != ARCHIVE_OK)) {
        archive_write_free(a);
        return false;
    }
    if (archive_write_open_filename(a, path.c_str()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }
    for (const auto& spec : entries) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_copy_pathname(entry, spec.name.c_str());
        archive_entry_set_filetype(entry, spec.type);
        archive_entry_set_perm(entry, spec.type == AE_IFDIR ? 0755 : 0644);
        if (!spec.symlink.empty()) archive_entry_copy_symlink(entry, spec.symlink.c_str());
        if (!spec.hardlink.empty()) archive_entry_copy_hardlink(entry, spec.hardlink.c_str());
        archive_entry_set_size(entry, spec.type == AE_IFREG && spec.hardlink.empty() ? static_cast<la_int64_t>(spec.data.size()) : 0);
        if (archive_write_header(a, entry) == ARCHIVE_OK && spec.type == AE_IFREG && !spec.data.empty()) {
            archive_write_data(a, spec.data.data(), spec.data.size());
        }
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
    return true;
}

}  // namespace akeno::test
