// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <optional>

#include "akeno/games/IGameDiscoveryProvider.hpp"
#include "akeno/shadowmount/ShadowMountClient.hpp"

namespace akeno::shadowmount {

games::GameInfo toGameInfo(const Game& game);

class ShadowMountGameProvider final : public games::IGameDiscoveryProvider {
public:
    explicit ShadowMountGameProvider(ShadowMountClient client) : client_(std::move(client)) {}

    std::string name() const override { return "ShadowMountPlus"; }
    Result<games::DiscoveryStatus> probe() override;
    Result<std::vector<games::GameInfo>> discoverGames() override;
    Result<std::string> loadIcon(const games::GameInfo& game, games::IconSize size) override;

    ShadowMountClient& client() noexcept { return client_; }

private:
    Result<VersionInfo> ensureVersion();

    std::mutex mutex_;
    ShadowMountClient client_;
    std::optional<VersionInfo> version_;
};

}  // namespace akeno::shadowmount
