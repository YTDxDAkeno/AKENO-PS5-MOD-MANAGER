// SPDX-License-Identifier: GPL-3.0-or-later
// Owns and wires the application services. Created once at startup, used by both the
// headless modes and the user interface.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "akeno/app/CommandLine.hpp"
#include "akeno/app/SystemCheck.hpp"
#include "akeno/core/AppPaths.hpp"
#include "akeno/core/OperationJournal.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/database/SettingsStore.hpp"
#include "akeno/downloads/DownloadManager.hpp"
#include "akeno/games/GameLibrary.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/network/CurlHttpClient.hpp"
#include "akeno/platform/Platform.hpp"
#include "akeno/providers/AkenoCatalogProvider.hpp"
#include "akeno/providers/GameBananaProvider.hpp"
#include "akeno/providers/NexusProvider.hpp"
#include "akeno/security/SafeFs.hpp"
#include "akeno/shadowmount/ShadowMountGameProvider.hpp"

namespace akeno::app {

class AppContext {
public:
    // Performs startup steps 1–5 of docs/architecture.md. Fails only if the application
    // directory itself cannot be prepared; every other problem is recorded and reported by the
    // system check instead.
    static Result<std::unique_ptr<AppContext>> create(const CommandLine& commandLine,
                                                      std::unique_ptr<platform::IPlatform> platform);
    ~AppContext();

    platform::IPlatform& platform() { return *platform_; }
    const AppPaths& paths() const { return paths_; }
    const security::SafeFs& fs() const { return *fs_; }
    database::Database* database() { return db_.get(); }
    const std::optional<Error>& databaseError() const { return dbError_; }
    games::GameLibrary* library() { return library_.get(); }
    games::IGameDiscoveryProvider* gameProvider() { return gameProvider_.get(); }
    network::IHttpClient& http() { return *http_; }
    // The Akeno Catalogue provider; null if its address is invalid (see catalogueError()).
    // Shared so background jobs keep a provider alive while the address is being changed.
    std::shared_ptr<providers::AkenoCatalogProvider> catalogue() const { return catalogue_; }
    // Nexus Mods, when the user placed a personal API key in <data>/nexus-apikey.txt.
    std::shared_ptr<providers::NexusProvider> nexus() const { return nexus_; }
    // GameBanana; used only while the setting is on.
    std::shared_ptr<providers::GameBananaProvider> gameBanana() const { return gameBanana_; }
    const std::optional<Error>& catalogueError() const { return catalogueError_; }
    // True when --catalogue-url set the address for this session (the setting is then ignored).
    bool catalogueOverridden() const { return catalogueOverride_.has_value(); }
    // The download engine. Created at startup but only started by the user interface, so the
    // headless modes never resume transfers.
    downloads::DownloadManager& downloads() { return *downloads_; }
    logging::RingBufferSink& logRing() { return *logRing_; }
    OperationJournal& journal() { return *journal_; }
    const std::optional<OperationState>& interruptedOperation() const { return interrupted_; }
    void clearInterruptedOperation() { interrupted_.reset(); }

    const database::Settings& settings() const { return settings_; }
    // Persists settings. Changes to the ShadowMountPlus port apply after a restart.
    Status saveSettings(const database::Settings& settings);

    SystemChecker makeSystemChecker();

    // Creates the catalogue provider for `url` (https, or http on loopback).
    void configureCatalogue(const std::string& url);

    // Writes logs/diagnostic-<timestamp>.txt: versions, settings, the last system report, the
    // interrupted operation (if any) and recent log lines. Secrets are already redacted.
    Result<std::filesystem::path> exportDiagnostics(const SystemReport* lastReport);

    // Writes a text report into logs/ and returns its path.
    Result<std::filesystem::path> writeReport(std::string_view prefix, std::string_view text);

private:
    AppContext() = default;

    std::unique_ptr<platform::IPlatform> platform_;
    AppPaths paths_;
    std::unique_ptr<security::SafeFs> fs_;
    std::shared_ptr<logging::RingBufferSink> logRing_;
    std::shared_ptr<logging::RotatingFileSink> fileSink_;
    std::unique_ptr<OperationJournal> journal_;
    std::optional<OperationState> interrupted_;
    std::unique_ptr<database::Database> db_;
    std::optional<Error> dbError_;
    database::Settings settings_;
    std::unique_ptr<network::CurlGlobal> curl_;
    std::unique_ptr<network::CurlHttpClient> http_;
    std::unique_ptr<shadowmount::ShadowMountGameProvider> gameProvider_;
    std::optional<Error> gameProviderError_;
    std::unique_ptr<games::GameLibrary> library_;
    std::unique_ptr<downloads::DownloadManager> downloads_;
    std::shared_ptr<providers::AkenoCatalogProvider> catalogue_;
    std::shared_ptr<providers::NexusProvider> nexus_;
    std::shared_ptr<providers::GameBananaProvider> gameBanana_;
    std::optional<Error> catalogueError_;
    std::optional<std::string> catalogueOverride_;  // --catalogue-url for this session
};

}  // namespace akeno::app
