// SPDX-License-Identifier: GPL-3.0-or-later
// CityHash64 (Google CityHash 1.1, MIT licensed algorithm, reimplemented here). Unreal Engine
// derives a package id (FPackageId) from the lower-case UTF-16 package name with it; Akeno uses
// it only to check that an IoStore package's chunk id matches the name stored inside it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace akeno::unreal {

std::uint64_t cityHash64(const char* data, std::size_t length);
inline std::uint64_t cityHash64(std::string_view data) { return cityHash64(data.data(), data.size()); }

// FPackageId::FromName for an ASCII package name ("/Game/Folder/Asset"). Returns 0 when the
// name contains non-ASCII characters (their lower-casing rules are not reproduced here).
std::uint64_t packageIdFromName(std::string_view packageName);

}  // namespace akeno::unreal
