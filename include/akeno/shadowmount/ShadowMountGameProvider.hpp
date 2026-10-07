// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <optional>

#include "akeno/games/ParamJson.hpp"
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
    // Where the system keeps a copy of each title's param.json (tests point it elsewhere).
    void setAppmetaBase(std::filesystem::path base) { appmetaBase_ = std::move(base); }

private:
    Result<VersionInfo> ensureVersion();

    std::mutex mutex_;
    ShadowMountClient client_;
    std::optional<VersionInfo> version_;
    std::filesystem::path appmetaBase_{std::string(games::kDefaultAppmetaBase)};
};

}  // namespace akeno::shadowmount
