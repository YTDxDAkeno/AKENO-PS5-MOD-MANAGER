// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/app/AppContext.hpp"

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Embedded.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/database/Migrations.hpp"
#include "akeno/logging/Redactor.hpp"

namespace akeno::app {

namespace fs = std::filesystem;
using logging::logger;

AppContext::~AppContext() {
    downloads_.reset();  // stops the worker before the HTTP client and database go away
    catalogue_.reset();
    library_.reset();
    gameProvider_.reset();
    http_.reset();
    curl_.reset();
    db_.reset();
    logger().info("startup", "shutting down");
    logger().clearSinks();
}

Result<std::unique_ptr<AppContext>> AppContext::create(const CommandLine& commandLine,
                                                       std::unique_ptr<platform::IPlatform> platform) {
    std::unique_ptr<AppContext> ctx(new AppContext());
    ctx->platform_ = std::move(platform);

    // 1. Application directory and write guard. The guard resolves symlinks in the root once
    //    (e.g. /data -> /user/data), and every path below is derived from the resolved root.
    fs::path requestedRoot = commandLine.dataRoot.value_or(ctx->platform_->defaultDataRoot());
    auto guard = security::WriteGuard::create({requestedRoot});
    if (!guard) {
        return std::move(guard).error();
    }
    ctx->paths_.root = guard->roots().front();
    ctx->fs_ = std::make_unique<security::SafeFs>(std::move(guard).value());

    // 2. Storage layout.
    for (const fs::path& directory : ctx->paths_.layout()) {
        auto created = ctx->fs_->createDirectories(directory);
        if (!created) {
            return std::move(created).error();
        }
    }

    // 3. Logging.
    ctx->logRing_ = std::make_shared<logging::RingBufferSink>(limits::kLogRingCapacity);
    ctx->fileSink_ = std::make_shared<logging::RotatingFileSink>(ctx->paths_.logs(), limits::kMaxLogFileBytes,
                                                                 limits::kMaxLogFiles);
    logger().clearSinks();
    logger().addSink(ctx->fileSink_);
    logger().addSink(ctx->logRing_);
    logger().addSink(std::make_shared<logging::StderrSink>());
    logger().setMinimumLevel(commandLine.verbose ? logging::LogLevel::Debug : logging::LogLevel::Info);
    logger().info("startup", strings::concat("Akeno PS5 Mod Manager ", build::version(), " (", build::target(), ", ",
                                             build::gitRevision(), ") on ", ctx->platform_->name()));
    logger().info("startup", "data directory: " + ctx->paths_.root.string());

    // 4. Interrupted operations.
    ctx->journal_ = std::make_unique<OperationJournal>(ctx->paths_.operationJournal(), *ctx->fs_);
    auto interrupted = ctx->journal_->load();
    if (!interrupted) {
        logger().error("recovery", "could not read the recovery journal: " + interrupted.error().describe());
        // A damaged journal is treated as an interrupted operation of unknown kind that may have
        // touched the overlay: the conservative assumption.
        OperationState unknown;
        unknown.operationId = "unknown";
        unknown.kind = "unknown";
        unknown.description = "An operation whose journal could not be read.";
        unknown.activeOverlayTouched = true;
        ctx->interrupted_ = unknown;
    } else if (interrupted->has_value()) {
        ctx->interrupted_ = interrupted->value();
        logger().warn("recovery", "interrupted operation found: " + ctx->interrupted_->operationId + " (" +
                                      ctx->interrupted_->kind + ", step '" + ctx->interrupted_->step + "')");
    } else {
        // Nothing is in progress, so Akeno's own entries in staging are leftovers.
        for (const auto& name : removeLeftoverStaging(*ctx->fs_, ctx->paths_.staging())) {
            logger().info("recovery", "removed a leftover staging entry: " + name);
        }
    }

    // 5. Database: open, back up, migrate, load settings.
    const fs::path dbFile = ctx->paths_.databaseFile();
    if (auto checked = ctx->fs_->guard().checkWritable(dbFile); !checked) {
        ctx->dbError_ = checked.error();
    } else if (auto opened = database::Database::open(dbFile); !opened) {
        ctx->dbError_ = opened.error();
    } else {
        std::unique_ptr<database::Database> db = std::move(opened).value();
        auto backup = [&](int fromVersion) -> Result<fs::path> {
            fs::path target = ctx->paths_.backups() /
                              strings::concat("akeno-", strings::utcTimestampCompact(), "-v", fromVersion, ".sqlite");
            AKENO_TRY(ctx->fs_->copyFile(dbFile, target));
            return target;
        };
        auto migrated = [&] {
            std::lock_guard<std::mutex> lock(db->mutex());
            return database::migrate(*db, database::builtinMigrations(), backup);
        }();
        if (!migrated) {
            ctx->dbError_ = migrated.error();
        } else {
            if (!migrated->backupFile.empty()) {
                logger().info("database", "backup before upgrade: " + migrated->backupFile.string());
            }
            ctx->db_ = std::move(db);
        }
    }
    if (ctx->dbError_) {
        logger().error("database", ctx->dbError_->describe());
    }
    if (ctx->db_) {
        database::SettingsStore store(*ctx->db_);
        auto loaded = store.load();
        if (loaded) {
            ctx->settings_ = loaded->settings;
            for (const auto& warning : loaded->warnings) {
                logger().warn("settings", warning);
            }
        } else {
            logger().error("settings", loaded.error().describe());
        }
    }
    if (ctx->settings_.debugLogging) {
        logger().setMinimumLevel(logging::LogLevel::Debug);
    }

    // 6. Network and ShadowMountPlus.
    ctx->curl_ = std::make_unique<network::CurlGlobal>();
    if (!ctx->curl_->ok()) {
        logger().error("network", "libcurl initialisation failed");
    }
    network::CurlClientOptions httpOptions;
    httpOptions.caBundlePem = embedded::caBundlePem();
    httpOptions.userAgent = strings::concat("AkenoModManager/", build::version());
    ctx->http_ = std::make_unique<network::CurlHttpClient>(httpOptions);
    logger().info("network", network::curlVersionDescription() +
                                 (httpOptions.caBundlePem.empty() ? ", system CA store" : ", embedded CA bundle"));

    shadowmount::Endpoint endpoint;
    endpoint.port = static_cast<std::uint16_t>(commandLine.shadowMountPort.value_or(ctx->settings_.shadowMountPort));
    auto client = shadowmount::ShadowMountClient::create(*ctx->http_, endpoint);
    if (!client) {
        ctx->gameProviderError_ = client.error();
        logger().error("shadowmount", client.error().describe());
    } else {
        ctx->gameProvider_ = std::make_unique<shadowmount::ShadowMountGameProvider>(std::move(client).value());
        ctx->library_ = std::make_unique<games::GameLibrary>(*ctx->gameProvider_, ctx->db_.get());
    }

    // 7. Mod catalogue.
    ctx->catalogueOverride_ = commandLine.catalogueUrl;
    ctx->configureCatalogue(commandLine.catalogueUrl.value_or(ctx->settings_.catalogueUrl));

    // 8. Download engine (started later by the user interface).
    downloads::DownloadManagerOptions downloadOptions;
    downloadOptions.directory = ctx->paths_.downloads();
    ctx->downloads_ =
        std::make_unique<downloads::DownloadManager>(*ctx->http_, *ctx->fs_, ctx->db_.get(), std::move(downloadOptions));
    return ctx;
}

void AppContext::configureCatalogue(const std::string& url) {
    auto provider = providers::AkenoCatalogProvider::create(*http_, url);
    if (!provider) {
        catalogueError_ = provider.error();
        catalogue_.reset();
        logger().error("catalogue", provider.error().describe());
        return;
    }
    catalogueError_.reset();
    catalogue_ = std::shared_ptr<providers::AkenoCatalogProvider>(std::move(provider).value());
    logger().info("catalogue", "catalogue address: " + catalogue_->baseUrl());
}

Status AppContext::saveSettings(const database::Settings& settings) {
    if (!db_) {
        return makeError(ErrorCode::Unavailable, "Settings cannot be saved because the database is unavailable.",
                         dbError_ ? dbError_->describe() : "");
    }
    database::SettingsStore store(*db_);
    AKENO_TRY(store.save(settings));
    const bool catalogueChanged = settings.catalogueUrl != settings_.catalogueUrl;
    settings_ = settings;
    if (catalogueChanged && !catalogueOverride_) {
        configureCatalogue(settings.catalogueUrl);
    }
    logger().setMinimumLevel(settings.debugLogging ? logging::LogLevel::Debug : logging::LogLevel::Info);
    logger().info("settings", "settings saved");
    return {};
}

SystemChecker AppContext::makeSystemChecker() {
    SystemCheckDependencies deps;
    deps.platform = platform_.get();
    deps.gameProvider = gameProvider_.get();
    deps.gameProviderError = gameProviderError_;
    deps.http = http_.get();
    deps.fs = fs_.get();
    deps.paths = paths_;
    deps.db = db_.get();
    deps.databaseError = dbError_;
    deps.networkProbeUrl = settings_.networkProbeUrl;
    deps.build = BuildFeatures{};
    deps.build.modBrowsing = true;  // Phase 2
    deps.build.downloading = true;   // Phase 3
    deps.build.installation = true;  // Phase 5
    return SystemChecker(std::move(deps));
}

Result<fs::path> AppContext::writeReport(std::string_view prefix, std::string_view text) {
    fs::path file = paths_.logs() / strings::concat(prefix, "-", strings::utcTimestampCompact(), ".txt");
    AKENO_TRY(fs_->writeFileAtomic(file, text));
    return file;
}

Result<fs::path> AppContext::exportDiagnostics(const SystemReport* lastReport) {
    std::string text;
    text += strings::concat("Akeno PS5 Mod Manager diagnostic report\n", "Generated: ", strings::utcTimestamp(), "\n");
    text += strings::concat("Version: ", build::version(), " (", build::target(), ", ", build::gitRevision(), ")\n");
    text += strings::concat("Compiler: ", build::compiler(), "\n");
    text += strings::concat("Platform: ", platform_->name(), "\n");
    text += strings::concat("Firmware: ", platform_->firmware().display, "\n");
    text += strings::concat("Network: ", network::curlVersionDescription(), "\n");
    text += strings::concat("Database: ", database::sqliteVersion(),
                            dbError_ ? " - ERROR: " + dbError_->describe() : std::string(), "\n");
    text += strings::concat("Data directory: ", paths_.root.string(), "\n");
    text += strings::concat("Testing status: ", build::testingStatus(), "\n\n");

    text += "Settings:\n";
    text += strings::concat("  shadowmount.port = ", settings_.shadowMountPort, "\n");
    text += strings::concat("  library.show_ps4 = ", settings_.showPs4Games, "\n");
    text += strings::concat("  library.show_homebrew = ", settings_.showHomebrew, "\n");
    text += strings::concat("  library.sort = ", database::toString(settings_.librarySort), "\n");
    text += strings::concat("  network.probe_url = ", settings_.networkProbeUrl, "\n");
    text += strings::concat("  catalogue.url = ", catalogue_ ? catalogue_->baseUrl() : settings_.catalogueUrl,
                            catalogueOverride_ ? " (command line)" : "", "\n\n");

    if (interrupted_) {
        text += strings::concat("Interrupted operation: ", interrupted_->operationId, " kind=", interrupted_->kind,
                                " step=", interrupted_->step, "\n\n");
    }
    if (lastReport != nullptr) {
        text += lastReport->toText();
        text += "\n";
    }
    text += "Recent log:\n";
    for (const auto& record : logRing_->snapshot()) {
        text += record.format();
        text += "\n";
    }
    auto file = writeReport("diagnostic", logging::redactSecrets(text));
    if (file) {
        logger().info("diagnostics", "diagnostic report written: " + file->string());
    }
    return file;
}

}  // namespace akeno::app
