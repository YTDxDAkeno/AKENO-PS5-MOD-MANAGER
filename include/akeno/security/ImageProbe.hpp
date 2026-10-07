// SPDX-License-Identifier: GPL-3.0-or-later
// Reads image dimensions from PNG/JPEG headers so oversized images ("decompression bombs")
// are rejected before any decoder allocates pixel memory.
#pragma once

#include <cstdint>
#include <string_view>

#include "akeno/core/Result.hpp"

namespace akeno::security {

enum class ImageFormat { Png, Jpeg };

struct ImageInfo {
    ImageFormat format = ImageFormat::Png;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Result<ImageInfo> probeImage(std::string_view bytes);

// probeImage plus the limits from core/Limits.hpp (bytes, dimension, pixel count).
Result<ImageInfo> validateImage(std::string_view bytes);

}  // namespace akeno::security
