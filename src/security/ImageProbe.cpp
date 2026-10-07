// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/ImageProbe.hpp"

#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"

namespace akeno::security {

namespace {

std::uint32_t readBe32(std::string_view bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3]));
}

std::uint32_t readBe16(std::string_view bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1]));
}

Error invalid(std::string detail) {
    return makeError(ErrorCode::Unsupported, "The image is not a valid PNG or JPEG file.", std::move(detail));
}

Result<ImageInfo> probePng(std::string_view bytes) {
    // Signature (8) + IHDR length (4) + "IHDR" (4) + width (4) + height (4).
    if (bytes.size() < 24) {
        return invalid("truncated PNG header");
    }
    if (bytes.substr(12, 4) != "IHDR" || readBe32(bytes, 8) != 13) {
        return invalid("PNG does not start with IHDR");
    }
    return ImageInfo{ImageFormat::Png, readBe32(bytes, 16), readBe32(bytes, 20)};
}

Result<ImageInfo> probeJpeg(std::string_view bytes) {
    std::size_t pos = 2;  // after SOI
    // Walk the marker segments until a start-of-frame marker carries the dimensions.
    for (int segments = 0; segments < 512; ++segments) {
        while (pos < bytes.size() && static_cast<unsigned char>(bytes[pos]) != 0xFF) ++pos;
        while (pos < bytes.size() && static_cast<unsigned char>(bytes[pos]) == 0xFF) ++pos;
        if (pos >= bytes.size()) break;
        const auto marker = static_cast<unsigned char>(bytes[pos]);
        ++pos;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;  // markers without a length field
        }
        if (marker == 0xD9 || marker == 0xDA) {
            break;  // end of image or start of scan before any frame header
        }
        if (pos + 2 > bytes.size()) break;
        const std::uint32_t length = readBe16(bytes, pos);
        if (length < 2 || pos + length > bytes.size()) break;
        const bool startOfFrame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (startOfFrame) {
            if (length < 7) break;
            const std::uint32_t height = readBe16(bytes, pos + 3);
            const std::uint32_t width = readBe16(bytes, pos + 5);
            return ImageInfo{ImageFormat::Jpeg, width, height};
        }
        pos += length;
    }
    return invalid("no JPEG frame header found");
}

}  // namespace

Result<ImageInfo> probeImage(std::string_view bytes) {
    static constexpr std::string_view kPngSignature("\x89PNG\r\n\x1a\n", 8);
    if (bytes.size() >= 8 && bytes.substr(0, 8) == kPngSignature) {
        return probePng(bytes);
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8 && static_cast<unsigned char>(bytes[2]) == 0xFF) {
        return probeJpeg(bytes);
    }
    return invalid("unknown image format");
}

Result<ImageInfo> validateImage(std::string_view bytes) {
    if (bytes.size() > limits::kMaxImageBytes) {
        return makeError(ErrorCode::ResponseTooLarge, "The image file is too large.",
                         strings::concat(bytes.size(), " bytes"));
    }
    auto info = probeImage(bytes);
    if (!info) {
        return info;
    }
    if (info->width == 0 || info->height == 0 || info->width > limits::kMaxImageDimension ||
        info->height > limits::kMaxImageDimension ||
        static_cast<std::uint64_t>(info->width) * info->height > limits::kMaxImagePixels) {
        return makeError(ErrorCode::Unsupported, "The image dimensions are not allowed.",
                         strings::concat(info->width, "x", info->height));
    }
    return info;
}

}  // namespace akeno::security
