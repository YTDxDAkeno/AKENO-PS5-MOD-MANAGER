// SPDX-License-Identifier: GPL-3.0-or-later
// The only interface the rest of the application uses to learn about installed games.
#pragma once

#include <string>
#include <vector>

#include "akeno/core/Result.hpp"
#include "akeno/games/GameInfo.hpp"

namespace akeno::games {

enum class IconSize { Thumbnail, Full };

struct DiscoveryStatus {
    bool available = false;
    std::string providerName;      // "ShadowMountPlus"
    std::string providerVersion;   // "1.7"
    std::string summary;           // "connected (API v1)"
    bool supportsIcons = false;
};

class IGameDiscoveryProvider {
public:
    virtual ~IGameDiscoveryProvider() = default;

    virtual std::string name() const = 0;
    virtual Result<DiscoveryStatus> probe() = 0;
    virtual Result<std::vector<GameInfo>> discoverGames() = 0;
    // Encoded image bytes (PNG). NotFound if the game has no icon.
    virtual Result<std::string> loadIcon(const GameInfo& game, IconSize size) = 0;
};

}  // namespace akeno::games
