// SPDX-License-Identifier: GPL-3.0-or-later
// Files embedded into the executable at build time (see cmake/EmbedFile.cmake).
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace akeno::embedded {

// Mozilla CA bundle (PEM). Empty in host builds, which use the system trust store.
std::string_view caBundlePem();

// DejaVu Sans fonts used by the user interface.
std::span<const unsigned char> fontRegular();
std::span<const unsigned char> fontBold();

}  // namespace akeno::embedded
