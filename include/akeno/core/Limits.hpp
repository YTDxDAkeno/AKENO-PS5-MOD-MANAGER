// SPDX-License-Identifier: GPL-3.0-or-later
// Central list of size and count limits applied to untrusted input.
// Every limit is enforced before memory is allocated for the data it guards.
#pragma once

#include <cstddef>
#include <cstdint>

namespace akeno::limits {

inline constexpr std::size_t KiB = 1024;
inline constexpr std::size_t MiB = 1024 * KiB;

// JSON documents
inline constexpr std::size_t kMaxJsonSmallResponse = 256 * KiB;   // version, settings, storage
inline constexpr std::size_t kMaxJsonGameList = 16 * MiB;         // ShadowMount game snapshot
inline constexpr std::size_t kMaxJsonCatalogue = 8 * MiB;         // one catalogue document (Phase 2)
inline constexpr int kMaxJsonDepth = 64;

// Images
inline constexpr std::size_t kMaxImageBytes = 4 * MiB;
inline constexpr std::uint32_t kMaxImageDimension = 4096;          // width or height, pixels
inline constexpr std::uint64_t kMaxImagePixels = 4096ull * 4096ull;

// Paths and names
inline constexpr std::size_t kMaxPathBytes = 1023;                 // ShadowMountPlus MAX_PATH - 1
inline constexpr std::size_t kMaxFileNameBytes = 255;
inline constexpr std::size_t kMaxDisplayStringBytes = 512;         // titles, names shown in the UI

// HTTP
inline constexpr long kDefaultConnectTimeoutMs = 5000;
inline constexpr long kDefaultTotalTimeoutMs = 20000;
inline constexpr int kMaxRedirects = 5;

// Logs
inline constexpr std::size_t kMaxLogFileBytes = 1 * MiB;
inline constexpr int kMaxLogFiles = 3;
inline constexpr std::size_t kLogRingCapacity = 400;
inline constexpr std::size_t kMaxLogLineBytes = 2048;

// Game library
inline constexpr std::size_t kMaxGames = 8192;                     // ShadowMountPlus MAX_IMAGE_TITLES

// Storage: never let Akeno fill the filesystem completely (Phase 3 uses this).
inline constexpr std::uint64_t kStorageSafetyReserveBytes = 2ull * 1024 * 1024 * 1024;

}  // namespace akeno::limits
