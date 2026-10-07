// SPDX-License-Identifier: GPL-3.0-or-later
// Persists download records in the `downloads` table (migration 2). Rows are validated when
// they are read back; a row that fails validation is skipped and reported, never trusted.
#pragma once

#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/downloads/DownloadTypes.hpp"

namespace akeno::downloads {

struct LoadedDownloads {
    std::vector<DownloadRecord> records;   // oldest first
    std::vector<std::string> warnings;
};

class DownloadStore {
public:
    explicit DownloadStore(database::Database& db) : db_(db) {}

    Result<LoadedDownloads> loadAll();
    // Inserts or replaces the whole record.
    Status save(const DownloadRecord& record);
    Status remove(const std::string& id);

private:
    database::Database& db_;
};

}  // namespace akeno::downloads
