// SPDX-License-Identifier: GPL-3.0-or-later
// Host builds rely on the operating system's trust store, so no CA bundle is embedded.
#include "akeno/core/Embedded.hpp"

namespace akeno::embedded {

std::string_view caBundlePem() { return {}; }

}  // namespace akeno::embedded
