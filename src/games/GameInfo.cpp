// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/games/GameInfo.hpp"

namespace akeno::games {

std::string_view toString(Platform platform) noexcept {
    switch (platform) {
        case Platform::Ps5: return "ps5";
        case Platform::Ps4: return "ps4";
        case Platform::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view toString(SourceType source) noexcept {
    switch (source) {
        case SourceType::Folder: return "folder";
        case SourceType::Image: return "image";
        case SourceType::Pkg: return "pkg";
        case SourceType::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view displayName(SourceType source) noexcept {
    switch (source) {
        case SourceType::Folder: return "Folder";
        case SourceType::Image: return "Disk image";
        case SourceType::Pkg: return "Installed package";
        case SourceType::Unknown: return "Unknown";
    }
    return "Unknown";
}

bool isValidTitleId(std::string_view titleId) noexcept {
    if (titleId.size() != 9) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (titleId[i] < 'A' || titleId[i] > 'Z') return false;
    }
    for (std::size_t i = 4; i < 9; ++i) {
        if (titleId[i] < '0' || titleId[i] > '9') return false;
    }
    return true;
}

TitleKind classifyTitleId(std::string_view titleId) noexcept {
    if (!isValidTitleId(titleId)) {
        return TitleKind::Other;
    }
    std::string_view prefix = titleId.substr(0, 4);
    if (prefix == "PPSA") return TitleKind::Ps5Game;
    if (prefix == "CUSA") return TitleKind::Ps4Game;
    if (prefix == "LAPY" || prefix == "FAKE") return TitleKind::Homebrew;
    return TitleKind::Other;
}

}  // namespace akeno::games
