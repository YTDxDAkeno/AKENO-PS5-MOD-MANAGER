// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/security/ImageProbe.hpp"
#include "akeno/security/SafeFs.hpp"

using namespace akeno;
using namespace akeno::security;

namespace {

std::string pngHeader(std::uint32_t width, std::uint32_t height) {
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    auto be32 = [&](std::uint32_t v) {
        bytes.push_back(static_cast<char>(v >> 24));
        bytes.push_back(static_cast<char>(v >> 16));
        bytes.push_back(static_cast<char>(v >> 8));
        bytes.push_back(static_cast<char>(v));
    };
    be32(13);
    bytes += "IHDR";
    be32(width);
    be32(height);
    bytes += std::string("\x08\x02\x00\x00\x00", 5);
    return bytes;
}

std::string jpegWithFrame(std::uint16_t width, std::uint16_t height) {
    std::string bytes("\xFF\xD8", 2);
    bytes += std::string("\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18);  // APP0
    bytes += std::string("\xFF\xC0\x00\x11\x08", 5);                                           // SOF0
    bytes.push_back(static_cast<char>(height >> 8));
    bytes.push_back(static_cast<char>(height));
    bytes.push_back(static_cast<char>(width >> 8));
    bytes.push_back(static_cast<char>(width));
    bytes += std::string(10, '\x01');
    return bytes;
}

}  // namespace

TEST_CASE("PNG dimensions come from the IHDR chunk") {
    auto info = probeImage(pngHeader(512, 256));
    REQUIRE(info.ok());
    CHECK(info->format == ImageFormat::Png);
    CHECK(info->width == 512);
    CHECK(info->height == 256);
}

TEST_CASE("the generated application icon is a valid 512x512 PNG") {
    auto bytes = readFileBounded(std::string(AKENO_TEST_FIXTURES) + "/../../assets/icon0.png", 1 << 20);
    REQUIRE(bytes.ok());
    auto info = validateImage(bytes.value());
    REQUIRE(info.ok());
    CHECK(info->width == 512);
    CHECK(info->height == 512);
}

TEST_CASE("JPEG dimensions come from the start-of-frame segment") {
    auto info = probeImage(jpegWithFrame(1920, 1080));
    REQUIRE(info.ok());
    CHECK(info->format == ImageFormat::Jpeg);
    CHECK(info->width == 1920);
    CHECK(info->height == 1080);
}

TEST_CASE("decompression bombs and garbage are rejected before decoding") {
    CHECK_FALSE(validateImage(pngHeader(100000, 100000)).ok());
    CHECK_FALSE(validateImage(pngHeader(0, 10)).ok());
    CHECK_FALSE(validateImage(pngHeader(4096, 4097)).ok());
    CHECK(validateImage(pngHeader(4096, 4096)).ok());
    CHECK_FALSE(validateImage("GIF89a....").ok());
    CHECK_FALSE(validateImage(std::string("\x89PNG\r\n\x1a\n", 8)).ok());   // truncated
    CHECK_FALSE(validateImage(std::string("\xFF\xD8\xFF\xD9", 4)).ok());     // no frame
    CHECK_FALSE(validateImage(std::string(5 * 1024 * 1024, 'x')).ok());     // too many bytes
    std::string badLength("\xFF\xD8\xFF\xE0\xFF\xFF", 6);                     // segment longer than data
    CHECK_FALSE(validateImage(badLength).ok());
}
