// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/AppController.hpp"
#include "akeno/diagnostics/DiagnosticExport.hpp"

#include <algorithm>
#include <mutex>
#include <system_error>

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/games/GameLibrary.hpp"
#include "akeno/install/OverlayManager.hpp"
#include "akeno/mods/Catalog.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/mods/ModPreparation.hpp"
#include "akeno/network/CurlHttpClient.hpp"
#include "akeno/security/ImageProbe.hpp"
#include "akeno/security/SafeName.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::ui {

using logging::logger;

namespace {

// Remote image cache bounds (cache/images). The oldest files are removed at startup.
constexpr std::size_t kMaxCachedImages = 600;
constexpr std::uint64_t kMaxImageCacheBytes = 160ull * 1024 * 1024;

std::string_view statusLabel(providers::CompatibilityStatus status) {
    switch (status) {
        case providers::CompatibilityStatus::Verified: return "VERIFIED";
        case providers::CompatibilityStatus::Likely: return "LIKELY";
        case providers::CompatibilityStatus::Experimental: return "EXPERIMENTAL";
        case providers::CompatibilityStatus::Unknown: return "UNKNOWN";
        case providers::CompatibilityStatus::PcOnly: return "PC ONLY";
        case providers::CompatibilityStatus::Incompatible: return "INCOMPATIBLE";
    }
    return "UNKNOWN";
}

// Turns a mod into a download request. The compatibility rules are applied here, whatever the
// screen offered: mods that may not be installed on PS5 are never downloaded for installing.
Result<downloads::DownloadRequest> buildDownloadRequest(providers::IModProvider& provider,
                                                        const providers::ModRef& ref,
                                                        const std::optional<providers::GameContext>& game,
                                                        bool confirmed) {
    auto details = provider.getModDetails(ref, game, nullptr);
    if (!details) {
        return std::move(details).error();
    }
    if (!details->installable) {
        const std::string reason =
            details->compatibilityReasons.empty() ? std::string(statusLabel(details->summary.compatibility))
                                                  : details->compatibilityReasons.front();
        return makeError(ErrorCode::SafetyViolation, "Akeno will not download this mod: " + reason);
    }
    if (details->needsConfirmation && !confirmed) {
        return makeError(ErrorCode::InvalidArgument,
                         "This mod is EXPERIMENTAL for your game version. Confirm the download first.");
    }
    if (details->files.empty()) {
        return makeError(ErrorCode::NotFound, "No downloadable file (zip or 7z) is listed for this mod.");
    }
    const providers::ModFile* file = &details->files.front();
    for (const auto& candidate : details->files) {
        if (candidate.primary) file = &candidate;
    }
    auto ticket = provider.resolveDownload(ref, *file, nullptr);
    if (!ticket) {
        return std::move(ticket).error();
    }
    if (!ticket->headers.empty()) {
        return makeError(ErrorCode::Unsupported, "Downloads that need extra request headers are not supported yet.");
    }
    downloads::DownloadRequest request;
    request.mod = ref;
    request.displayName = details->summary.name;
    request.modVersion = details->summary.version;
    if (game) {
        request.gameTitleId = game->titleId;
        request.gameVersion = game->version;
    }
    request.compatibility = std::string(statusLabel(details->summary.compatibility));
    request.url = ticket->url;
    request.expectedSize = ticket->expectedSize;
    request.expectedSha256 = ticket->expectedSha256;
    request.format = mods::parseArchiveFormat(ticket->format);
    request.catalogueInstallable = details->installable;
    // Only the Akeno Catalogue describes how an archive maps onto the game; other providers'
    // archives are installed as they are laid out (shown by the check before installing).
    if (auto* catalogue = dynamic_cast<providers::AkenoCatalogProvider*>(&provider)) {
        auto manifest = catalogue->manifest(ref, nullptr);
        if (!manifest) {
            return std::move(manifest).error();
        }
        request.archiveRoot = manifest->archiveRoot;
        request.targetPrefix = manifest->targetPrefix;
    }
    return request;
}

providers::CompatibilityStatus statusFromLabel(std::string_view label) {
    for (auto status : {providers::CompatibilityStatus::Verified, providers::CompatibilityStatus::Likely,
                        providers::CompatibilityStatus::Experimental, providers::CompatibilityStatus::PcOnly,
                        providers::CompatibilityStatus::Incompatible}) {
        if (statusLabel(status) == label) return status;
    }
    return providers::CompatibilityStatus::Unknown;
}

bool sameGame(const std::optional<providers::GameContext>& a, const std::optional<providers::GameContext>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->titleId == b->titleId && a->version == b->version);
}

}  // namespace

AppController::AppController(app::AppContext& context, TaskRunner& tasks, MainThreadQueue& mainQueue,
                             IImageLoader* images)
    : context_(context), tasks_(tasks), mainQueue_(mainQueue), images_(images) {
    state_.settings = context_.settings();
    state_.settingsPersistent = context_.database() != nullptr;
    if (const auto& interrupted = context_.interruptedOperation()) {
        state_.recovery = RecoveryView{*interrupted, adviseRecovery(*interrupted)};
    }
    AboutInfo& about = state_.about;
    about.version = std::string(build::version());
    about.revision = std::string(build::gitRevision());
    about.target = std::string(build::target());
    about.compiler = std::string(build::compiler());
    about.testingStatus = std::string(build::testingStatus());
    about.platformName = std::string(context_.platform().name());
    about.firmware = context_.platform().firmware().display;
    about.dataRoot = context_.paths().root.string();
    about.networkStack = network::curlVersionDescription();
    about.databaseEngine = database::sqliteVersion();
    if (context_.databaseError()) {
        about.databaseEngine += " - unavailable: " + context_.databaseError()->message;
    }
    resetCatalogView();
}

AppController::~AppController() {
    diagnosticsCancel_.cancel();
    alive_->store(false);
    context_.downloads().setListener(nullptr);
    context_.downloads().stop();
}

void AppController::start() {
    runSystemCheck();
    tasks_.submit([this] { pruneImageCache(); });

    // The worker reports changes on its own thread; refreshes are coalesced and run here.
    MainThreadQueue* queue = &mainQueue_;
    auto pending = downloadsRefreshPending_;
    auto alive = alive_;
    context_.downloads().setListener([this, queue, pending, alive] {
        if (pending->exchange(true)) return;
        queue->post([this, pending, alive] {
            pending->store(false);
            if (alive->load()) refreshDownloads();
        });
    });
    auto started = context_.downloads().start();
    if (!started) {
        logger().error("downloads", started.error().describe());
        addNotice("Downloads are unavailable: " + started.error().message, ToastKind::Error);
    }
    refreshDownloads();
    removeOrphanReports();
}

void AppController::removeOrphanReports() {
    // Check reports belong to downloads; remove those whose download is gone.
    std::error_code ec;
    const auto directory = context_.paths().cache() / "analysis";
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() != 21 || name.substr(16) != ".json") continue;
        const std::string id = name.substr(0, 16);
        if (!downloads::isValidDownloadId(id) || state_.downloads.find(id) != nullptr) continue;
        auto removed = context_.fs().removeFile(it->path());
        if (!removed) logger().warn("check", "could not remove " + name + ": " + removed.error().describe());
    }
}

void AppController::runSystemCheck() {
    if (state_.systemCheck.running) {
        return;
    }
    state_.systemCheck.running = true;
    state_.systemCheck.results.clear();
    // Built on the UI thread so it captures a consistent copy of the settings; it then runs on a
    // worker. The checker only uses thread-safe services.
    auto checker = std::make_shared<app::SystemChecker>(context_.makeSystemChecker());
    tasks_.submit([this, checker] {
        app::SystemReport report = checker->run([this](const app::CheckResult& result) {
            mainQueue_.post([this, result] { state_.systemCheck.results.push_back(result); });
        });
        mainQueue_.post([this, report = std::move(report)]() mutable {
            state_.systemCheck.running = false;
            state_.systemCheck.results = report.checks;
            const bool libraryAvailable = report.features.gameLibrary.state == app::FeatureState::Available;
            state_.systemCheck.report = std::move(report);
            if (libraryAvailable && !state_.library.everLoaded) {
                refreshLibrary();
            }
        });
    });
}

void AppController::refreshLibrary() {
    games::GameLibrary* library = context_.library();
    if (library == nullptr) {
        state_.library.error = makeError(ErrorCode::Unavailable, "ShadowMountPlus is not configured.");
        return;
    }
    if (state_.library.loading) {
        return;
    }
    state_.library.loading = true;
    tasks_.submit([this, library] {
        auto snapshot = library->refresh();
        mainQueue_.post([this, snapshot = std::move(snapshot)]() mutable {
            state_.library.loading = false;
            if (!snapshot) {
                state_.library.error = snapshot.error();
                logger().warn("ui", "library refresh failed: " + snapshot.error().describe());
                return;
            }
            state_.library.error.reset();
            state_.library.everLoaded = true;
            state_.library.refreshedAt = snapshot->refreshedAt;
            state_.library.versionChanges = snapshot->versionChanges;
            allGames_ = std::move(snapshot->games);
            if (images_ != nullptr) images_->retryFailed();
            applyFilter();
        });
    });
}

void AppController::applyFilter() {
    games::LibraryFilter filter;
    filter.includePs4 = state_.settings.showPs4Games;
    filter.includeHomebrew = state_.settings.showHomebrew;
    filter.sort = state_.settings.librarySort;
    state_.library.totalGames = allGames_.size();
    state_.library.games = games::applyFilter(allGames_, filter);
}

Status AppController::saveSettings(const database::Settings& settings) {
    const bool catalogueChanged = settings.catalogueUrl != state_.settings.catalogueUrl ||
                                  settings.gameBanana != state_.settings.gameBanana;
    AKENO_TRY(context_.saveSettings(settings));
    state_.settings = settings;
    applyFilter();
    if (catalogueChanged) {
        resetCatalogView();
    }
    return {};
}

void AppController::exportDiagnostics(const std::string& titleId, bool deep) {
    if (installing_ || state_.check.running || state_.diagnostics.running) {
        addNotice("Wait for the current operation, or cancel the diagnostic export.", ToastKind::Warning);
        return;
    }
    if (!titleId.empty() && !games::isValidTitleId(titleId)) {
        addNotice("The diagnostic title ID is invalid.", ToastKind::Error); return;
    }
    logger().flush();  // the export reads akeno.log: everything logged so far must be on disk
    diagnostics::Request request;
    request.paths = context_.paths();
    request.titleId = titleId;
    request.cachedGames = allGames_;
    request.firmware = context_.platform().firmware();
    request.mode = deep ? diagnostics::Mode::Deep : diagnostics::Mode::Quick;
    request.limits = deep ? diagnostics::deepLimits() : diagnostics::quickLimits();
    request.logFiles = {context_.paths().logs() / "akeno.log", context_.paths().logs() / "akeno.log.1"};
    for (int i = 2; i <= 4; ++i) request.logFiles.push_back(context_.paths().logs() / ("akeno.log." + std::to_string(i)));
    if (context_.platform().isConsole()) {
        request.smpConfigFile = "/data/shadowmount/config.ini";
        request.logFiles.emplace_back("/data/shadowmount/debug.log");
        request.logFiles.emplace_back("/data/shadowmount/debug.log.1");
    }
    const auto port = static_cast<std::uint16_t>(state_.settings.shadowMountPort);
    diagnosticsCancel_ = CancellationToken{};
    const auto cancel = diagnosticsCancel_;
    const auto alive = alive_;
    state_.diagnostics.running = true;
    state_.diagnostics.progress = deep ? "Starting deep read-only diagnostic export" : "Starting quick read-only diagnostic export";
    addNotice(deep ? "Exporting deep diagnostics (this can take several minutes). No mods will be activated."
                   : "Exporting diagnostics. No mods will be activated.",
              ToastKind::Info);
    tasks_.submit([this, request, port, cancel, alive] {
        Result<std::filesystem::path> result = makeError(ErrorCode::Internal, "Diagnostic export failed");
        try {
            auto client = shadowmount::ShadowMountClient::create(context_.http(), {"127.0.0.1", port});
            result = client ? diagnostics::exportReport(request, context_.fs(), *client, &cancel,
                [this, alive](std::string progress) {
                    mainQueue_.post([this, alive, progress = std::move(progress)] {
                        if (alive->load()) state_.diagnostics.progress = progress;
                    });
                }) : Result<std::filesystem::path>(client.error());
        } catch (const std::exception&) {
            // A malformed input or allocation failure must not leave the UI busy forever.
            result = makeError(ErrorCode::Internal, "Diagnostic export failed while reading or encoding evidence");
        }
        mainQueue_.post([this, alive, result = std::move(result), cancelled = cancel.cancelled()] {
            if (!alive->load()) return;
            state_.diagnostics.running = false;
            state_.diagnostics.progress = result ? (cancelled ? "Cancelled; partial report saved" : "Report saved; check completeness fields")
                                                 : "Export failed: " + result.error().message;
            if (result) state_.diagnostics.lastExport = result->string();
            addNotice(result ? "Diagnostics written to " + result->string() : state_.diagnostics.progress,
                      result ? ToastKind::Success : ToastKind::Error);
        });
    });
}

void AppController::cancelDiagnostics() {
    diagnosticsCancel_.cancel();
    if (state_.diagnostics.running) state_.diagnostics.progress = "Cancelling; preserving partial evidence";
}

std::vector<logging::LogRecord> AppController::recentLogs() { return context_.logRing().snapshot(); }

Status AppController::cleanInterruptedOperation() {
    if (state_.diagnostics.running) return makeError(ErrorCode::Busy, "Wait for the diagnostic export before recovery cleanup.");
    if (!state_.recovery) {
        return {};
    }
    AKENO_TRY(context_.journal().cleanStaging(state_.recovery->state, context_.paths().staging()));
    logger().info("recovery", "cleaned staging of interrupted operation " + state_.recovery->state.operationId);
    state_.recovery.reset();
    context_.clearInterruptedOperation();
    return {};
}

void AppController::postponeRecovery() {
    if (state_.recovery) {
        logger().info("recovery", "recovery postponed by the user: " + state_.recovery->state.operationId);
    }
}

Result<std::string> AppController::fetchIcon(const games::GameInfo& game) {
    // Runs on a worker. Icons are cached per title and version in cache/icons.
    const std::string fileName =
        game.titleId + "-" + security::toSafeFileComponent(game.version.empty() ? "unknown" : game.version) + ".png";
    const auto cacheFile = context_.paths().iconCache() / fileName;
    std::error_code ec;
    if (std::filesystem::exists(cacheFile, ec)) {
        auto cached = security::readFileBounded(cacheFile, limits::kMaxImageBytes);
        if (cached) {
            return cached;
        }
    }
    games::IGameDiscoveryProvider* provider = context_.gameProvider();
    if (provider == nullptr) {
        return makeError(ErrorCode::Unavailable, "No game provider.");
    }
    auto bytes = provider->loadIcon(game, games::IconSize::Full);
    if (!bytes) {
        return bytes;
    }
    auto stored = context_.fs().writeFileAtomic(cacheFile, bytes.value());
    if (!stored) {
        logger().warn("images", "could not cache icon: " + stored.error().describe());
    }
    return bytes;
}

std::string AppController::gameIconKey(const games::GameInfo& game, int size) {
    if (!game.hasIcon || !games::isValidTitleId(game.titleId)) {
        return {};
    }
    std::string key = strings::concat("icon:", game.titleId, ":", game.version, ":", size);
    if (images_ != nullptr) {
        images_->ensure(key, size, size, [this, game] { return fetchIcon(game); });
    }
    return key;
}

// ---------------------------------------------------------------- remote images

std::string AppController::remoteImageKey(const std::string& url, int width, int height) {
    if (width <= 0 || height <= 0 || width > 1920 || height > 1080) {
        return {};
    }
    auto known = urlKeys_.find(url);
    if (known == urlKeys_.end()) {
        // Only https (or http on this console for local testing) is ever requested.
        if (url.empty() || url.size() > 2048 || !mods::isAllowedRemoteUrl(url)) {
            return {};
        }
        if (urlKeys_.size() > 2000) urlKeys_.clear();
        known = urlKeys_.emplace(url, security::sha256Hex(url)).first;
    }
    std::string key = strings::concat("remote:", known->second.substr(0, 32), ":", width, "x", height);
    if (images_ != nullptr) {
        images_->ensure(key, width, height, [this, url] { return fetchRemoteImage(url); });
    }
    return key;
}

Result<std::string> AppController::fetchRemoteImage(const std::string& url) {
    // Runs on a worker. Images are validated before they are cached or decoded; the cache file
    // name is derived from the URL hash, never from anything the server sends.
    const auto cacheFile = context_.paths().imageCache() / (security::sha256Hex(url) + ".img");
    std::error_code ec;
    if (std::filesystem::exists(cacheFile, ec)) {
        auto cached = security::readFileBounded(cacheFile, limits::kMaxImageBytes);
        if (cached && security::validateImage(cached.value())) {
            return cached;
        }
        logger().debug("images", "ignoring an invalid cached image");
    }
    network::HttpRequest request;
    request.url = url;
    request.maxResponseBytes = limits::kMaxImageBytes;
    request.totalTimeoutMs = 30000;
    request.headers = {{"Accept", "image/png, image/jpeg"}};
    auto response = context_.http().send(request);
    if (!response) {
        return std::move(response).error();
    }
    if (!response->isSuccess()) {
        return makeError(ErrorCode::HttpStatus, "The image could not be downloaded.",
                         strings::concat("HTTP ", response->status));
    }
    auto info = security::validateImage(response->body);
    if (!info) {
        return std::move(info).error();
    }
    auto stored = context_.fs().writeFileAtomic(cacheFile, response->body);
    if (!stored) {
        logger().warn("images", "could not cache image: " + stored.error().describe());
    }
    return std::move(response->body);
}

void AppController::pruneImageCache() {
    // Runs on a worker at startup. Keeps the cache within its file and size limits by removing
    // the files that were downloaded first.
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        std::uint64_t size;
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(context_.paths().imageCache(), ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || it->is_symlink(entryEc)) continue;
        Entry entry{it->path(), it->last_write_time(entryEc), it->file_size(entryEc)};
        if (entryEc) continue;
        total += entry.size;
        entries.push_back(std::move(entry));
        if (entries.size() > 100000) break;
    }
    if (entries.size() <= kMaxCachedImages && total <= kMaxImageCacheBytes) {
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    std::size_t count = entries.size();
    std::size_t removed = 0;
    for (const auto& entry : entries) {
        if (count <= kMaxCachedImages && total <= kMaxImageCacheBytes) break;
        if (context_.fs().removeFile(entry.path)) {
            ++removed;
        }
        --count;
        total -= entry.size;
    }
    logger().info("images", strings::concat("image cache pruned: ", removed, " files removed"));
}

// ---------------------------------------------------------------- catalogue

void AppController::resetCatalogView() {
    // Results of requests made for the previous address are dropped.
    ++catalogGeneration_;
    listCancel_.cancel();
    detailCancel_.cancel();
    auto provider = context_.catalogue();
    CatalogView view;
    view.configured = provider != nullptr;
    view.source = provider ? provider->baseUrl() : state_.settings.catalogueUrl;
    view.overridden = context_.catalogueOverridden();
    view.configurationError = context_.catalogueError();
    state_.catalog = std::move(view);

    const std::uint64_t listRequest = state_.modList.request + 1;
    state_.modList = ModListView{};
    state_.modList.request = listRequest;
    const std::uint64_t detailRequest = state_.modDetail.request + 1;
    state_.modDetail = ModDetailView{};
    state_.modDetail.request = detailRequest;
}

std::shared_ptr<providers::IModProvider> AppController::providerFor(std::string_view providerId) const {
    if (providerId == providers::kNexusProviderId) return context_.nexus();
    if (providerId == providers::kGameBananaProviderId) {
        return state_.settings.gameBanana ? context_.gameBanana() : nullptr;
    }
    return context_.catalogue();
}

std::shared_ptr<providers::IModProvider> AppController::providerForGame(const std::string& providerGameId) const {
    if (strings::startsWith(providerGameId, providers::kNexusGamePrefix)) return context_.nexus();
    if (strings::startsWith(providerGameId, providers::kGameBananaGamePrefix)) {
        return state_.settings.gameBanana ? context_.gameBanana() : nullptr;
    }
    return context_.catalogue();
}

void AppController::loadCatalogGames(bool forceRefresh) {
    CatalogView& view = state_.catalog;
    auto provider = context_.catalogue();
    auto nexus = context_.nexus();
    auto banana = state_.settings.gameBanana ? context_.gameBanana() : nullptr;
    if (!provider && !nexus && !banana) {
        view.error = context_.catalogueError().value_or(
            makeError(ErrorCode::Unavailable, "The mod catalogue address is not valid."));
        return;
    }
    if (view.loading || (view.loaded && !forceRefresh)) {
        return;
    }
    view.loading = true;
    const std::uint64_t generation = catalogGeneration_;
    std::vector<providers::InstalledGameName> installed;
    for (const auto& game : allGames_) installed.push_back({game.titleId, game.name});
    if (nexus) nexus->setInstalledGames(installed);
    if (banana) banana->setInstalledGames(installed);
    tasks_.submit([this, provider, nexus, banana, forceRefresh, generation] {
        Result<std::vector<providers::ProviderGame>> games = std::vector<providers::ProviderGame>{};
        if (provider) {
            if (forceRefresh) provider->clearCache();
            games = provider->listGames(nullptr);
        }
        if (nexus) {
            auto more = nexus->listGames(nullptr);
            if (more) {
                if (!games) games = std::vector<providers::ProviderGame>{};  // Nexus still works
                for (auto& game : more.value()) games->push_back(std::move(game));
            } else {
                logger().warn("nexus", "could not load the Nexus Mods games: " + more.error().describe());
                if (!provider) games = more.error();
            }
        }
        if (banana) {
            auto more = banana->listGames(nullptr);
            if (more) {
                if (!games) games = std::vector<providers::ProviderGame>{};
                for (auto& game : more.value()) games->push_back(std::move(game));
            } else {
                logger().warn("gamebanana", "could not load the GameBanana games: " + more.error().describe());
                if (!provider && !nexus) games = more.error();
            }
        }
        mainQueue_.post([this, generation, games = std::move(games)]() mutable {
            if (generation != catalogGeneration_) return;
            CatalogView& current = state_.catalog;
            current.loading = false;
            if (!games) {
                current.error = games.error();
                logger().warn("catalogue", "could not load the catalogue: " + games.error().describe());
                return;
            }
            current.error.reset();
            current.loaded = true;
            current.games = std::move(games).value();
            logger().info("catalogue", strings::concat("catalogue loaded: ", current.games.size(), " games"));
        });
    });
}

void AppController::loadModList(const providers::SearchQuery& query) {
    ModListView& view = state_.modList;
    const bool sameList = view.page && view.query.providerGameId == query.providerGameId && sameGame(view.query.game, query.game);
    ++view.request;
    view.query = query;
    view.error.reset();
    if (!sameList) view.page.reset();
    auto provider = providerForGame(query.providerGameId);
    if (!provider) {
        view.loading = false;
        view.error = context_.catalogueError().value_or(
            makeError(ErrorCode::Unavailable, "The mod catalogue address is not valid."));
        return;
    }
    view.loading = true;
    listCancel_.cancel();
    listCancel_ = CancellationToken{};
    const CancellationToken cancel = listCancel_;
    const std::uint64_t request = view.request;
    const std::uint64_t generation = catalogGeneration_;
    tasks_.submit([this, provider, query, cancel, request, generation] {
        auto page = query.text.empty() ? provider->browseMods(query, &cancel) : provider->searchMods(query, &cancel);
        mainQueue_.post([this, request, generation, page = std::move(page)]() mutable {
            if (generation != catalogGeneration_ || request != state_.modList.request) return;
            ModListView& current = state_.modList;
            current.loading = false;
            if (!page) {
                current.error = page.error();
                current.page.reset();
                logger().warn("catalogue", "could not list mods: " + page.error().describe());
                return;
            }
            current.page = std::move(page).value();
        });
    });
}

void AppController::loadModDetails(const providers::ModRef& ref, const std::optional<providers::GameContext>& game) {
    ModDetailView& view = state_.modDetail;
    if (!(view.ref == ref)) view.details.reset();
    ++view.request;
    view.ref = ref;
    view.error.reset();
    auto provider = providerFor(ref.providerId);
    if (!provider) {
        view.loading = false;
        view.error = context_.catalogueError().value_or(
            makeError(ErrorCode::Unavailable, "The mod catalogue address is not valid."));
        return;
    }
    view.loading = true;
    detailCancel_.cancel();
    detailCancel_ = CancellationToken{};
    const CancellationToken cancel = detailCancel_;
    const std::uint64_t request = view.request;
    const std::uint64_t generation = catalogGeneration_;
    tasks_.submit([this, provider, ref, game, cancel, request, generation] {
        auto details = provider->getModDetails(ref, game, &cancel);
        mainQueue_.post([this, request, generation, details = std::move(details)]() mutable {
            if (generation != catalogGeneration_ || request != state_.modDetail.request) return;
            ModDetailView& current = state_.modDetail;
            current.loading = false;
            if (!details) {
                current.error = details.error();
                current.details.reset();
                logger().warn("catalogue", "could not load mod details: " + details.error().describe());
                return;
            }
            current.details = std::move(details).value();
        });
    });
}

// ---------------------------------------------------------------- downloads

void AppController::addNotice(std::string text, ToastKind kind) {
    state_.notices.push_back(Notice{++noticeSerial_, std::move(text), kind});
    if (state_.notices.size() > 8) state_.notices.erase(state_.notices.begin());
}

void AppController::refreshDownloads() {
    downloads::DownloadManager& manager = context_.downloads();
    std::vector<downloads::DownloadInfo> items = manager.snapshot();
    // Announce downloads that finished since the last refresh.
    for (const auto& item : items) {
        const downloads::DownloadInfo* before = state_.downloads.find(item.record.id);
        if (before == nullptr || before->record.state == item.record.state) continue;
        if (item.record.state == downloads::DownloadState::Completed) {
            addNotice("Downloaded and checked: " + item.record.request.displayName, ToastKind::Success);
        } else if (item.record.state == downloads::DownloadState::Failed) {
            addNotice("Download failed: " + item.record.request.displayName, ToastKind::Error);
        }
    }
    state_.downloads.started = manager.started();
    state_.downloads.items = std::move(items);
    state_.downloads.reserveBytes = limits::kStorageSafetyReserveBytes;
    auto space = security::queryStorageSpace(manager.directory());
    if (space) {
        state_.downloads.freeBytes = space->availableBytes;
    } else {
        state_.downloads.freeBytes.reset();
    }
}

void AppController::startDownload(const providers::ModRef& ref, const std::optional<providers::GameContext>& game,
                                  bool confirmed) {
    const auto& report = state_.systemCheck.report;
    if (report && report->features.downloading.state != app::FeatureState::Available) {
        addNotice("Downloading is not available: " + report->features.downloading.reason, ToastKind::Error);
        return;
    }
    auto provider = providerFor(ref.providerId);
    if (!provider) {
        addNotice("This mod source is not available.", ToastKind::Error);
        return;
    }
    downloads::DownloadManager* manager = &context_.downloads();
    auto alive = alive_;
    tasks_.submit([this, provider, manager, ref, game, confirmed, alive] {
        std::string text;
        ToastKind kind = ToastKind::Info;
        auto request = buildDownloadRequest(*provider, ref, game, confirmed);
        if (!request) {
            text = request.error().message;
            kind = ToastKind::Error;
            logger().warn("downloads", "download refused for " + ref.modId + ": " + request.error().describe());
        } else {
            const std::string name = request->displayName;
            auto queued = manager->enqueue(std::move(request).value());
            if (!queued) {
                text = "Could not add the download: " + queued.error().message;
                kind = ToastKind::Error;
            } else if (queued->alreadyPresent) {
                text = "Already in Downloads: " + name;
            } else {
                text = "Added to Downloads: " + name;
                kind = ToastKind::Success;
            }
        }
        mainQueue_.post([this, alive, text = std::move(text), kind]() mutable {
            if (!alive->load()) return;
            addNotice(std::move(text), kind);
            refreshDownloads();
        });
    });
}

Status AppController::pauseDownload(const std::string& id) {
    auto paused = context_.downloads().pause(id);
    refreshDownloads();
    return paused;
}

Status AppController::resumeDownload(const std::string& id) {
    auto resumed = context_.downloads().resume(id);
    refreshDownloads();
    return resumed;
}

Status AppController::removeDownload(const std::string& id) {
    if (state_.check.running && state_.check.downloadId == id) {
        return makeError(ErrorCode::Busy, "This download is being checked. Cancel the check first.");
    }
    auto removed = context_.downloads().remove(id);
    if (removed) {
        const auto report = mods::reportPath(context_.paths(), id);
        std::error_code ec;
        if (std::filesystem::exists(report, ec)) (void)context_.fs().removeFile(report);
        if (state_.check.downloadId == id) state_.check = ModCheckView{};
    }
    refreshDownloads();
    return removed;
}

// ---------------------------------------------------------------- checks (Phase 4)

void AppController::checkDownload(const std::string& id, bool again) {
    ModCheckView& view = state_.check;
    if (installing_ || state_.diagnostics.running) {
        addNotice("Wait until the mod change has finished.", ToastKind::Warning);
        return;
    }
    if (view.running) {
        if (view.downloadId != id) addNotice("Another mod is being checked. Wait for it to finish.", ToastKind::Warning);
        return;
    }
    view = ModCheckView{};
    view.downloadId = id;
    const downloads::DownloadInfo* item = state_.downloads.find(id);
    auto file = context_.downloads().completedFile(id);
    if (item == nullptr || !file) {
        view.error = item == nullptr ? makeError(ErrorCode::NotFound, "This download no longer exists.") : file.error();
        return;
    }
    const downloads::DownloadRequest& download = item->record.request;
    mods::ModCheckRequest request;
    request.downloadId = id;
    request.archive = file.value();
    request.format = download.format;
    request.mod = download.mod;
    request.displayName = download.displayName;
    request.modVersion = download.modVersion;
    request.titleId = download.gameTitleId;
    for (const auto& game : allGames_) {
        if (game.titleId != download.gameTitleId) continue;
        request.sourceType = game.sourceType;
        request.gameVersion = game.version;
        request.contentId = game.contentId;
        request.installedPkg = game.installedPkg;
        if (!game.installedPkg && game.sourceType == games::SourceType::Folder) request.gameFolder = game.installPath;
    }
    request.archiveRoot = download.archiveRoot;
    request.targetPrefix = download.targetPrefix;
    request.catalogueStatus = statusFromLabel(download.compatibility);
    request.catalogueInstallable = download.catalogueInstallable;
    request.pcSource = mods::isPcProvider(download.mod.providerId);
    request.curated = mods::isCuratedProvider(download.mod.providerId) && !request.pcSource;
    request.installedMods = installedModPaths(download.gameTitleId);
    const auto records = testRecords(download.gameTitleId);
    auto registry = std::make_shared<const compatibility::Registry>(install::registryWithReports(download.gameTitleId, records));
    request.registry = registry.get();
    request.localEvidence = install::evidenceStamp(records);
    const bool interrupted = context_.interruptedOperation().has_value();
    const std::optional<bool> hardLinks =
        state_.systemCheck.report ? state_.systemCheck.report->hardLinksSupported : std::nullopt;

    view.running = true;
    checkCancel_ = CancellationToken{};
    const CancellationToken cancel = checkCancel_;
    auto alive = alive_;
    // Progress from the worker is coalesced: only the latest value is posted.
    struct Progress {
        std::mutex mutex;
        mods::CheckPhase phase = mods::CheckPhase::Inspecting;
        double fraction = 0.0;
        bool pending = false;
    };
    auto shared = std::make_shared<Progress>();
    tasks_.submit([this, request, registry, again, cancel, interrupted, hardLinks, alive, shared] {
        Result<mods::ModCheckReport> result = makeError(ErrorCode::NotFound, "No stored result.");
        if (!again) result = mods::loadReport(context_.paths(), request.downloadId);
        // A stored result describes the game as it was: check again after a game update, and after
        // the user reported a test result for the game.
        if (result && result->game.version != request.gameVersion) {
            logger().info("check", request.downloadId + ": the game version changed since the check; checking again");
            result = makeError(ErrorCode::NotFound, "Stale result.");
        } else if (result && result->localEvidence != request.localEvidence) {
            logger().info("check", request.downloadId + ": test reports for the game changed since the check; checking again");
            result = makeError(ErrorCode::NotFound, "Stale result.");
        }
        if (!result) {
            mods::ModCheckEnvironment env{context_.fs(), context_.paths(), context_.journal(), interrupted,
                                          limits::kStorageSafetyReserveBytes, {}, {}};
            result = mods::runModCheck(request, env, &cancel, [this, alive, shared](mods::CheckPhase phase, double fraction) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->phase = phase;
                shared->fraction = fraction;
                if (shared->pending) return;
                shared->pending = true;
                mainQueue_.post([this, alive, shared] {
                    if (!alive->load()) return;
                    std::lock_guard<std::mutex> inner(shared->mutex);
                    shared->pending = false;
                    if (state_.check.running) {
                        state_.check.phase = shared->phase;
                        state_.check.progress = shared->fraction;
                    }
                });
            });
            if (result) {
                auto saved = mods::saveReport(context_.fs(), context_.paths(), result.value());
                if (!saved) logger().warn("check", "could not store the result: " + saved.error().describe());
            }
        }
        if (result) mods::completeReport(result.value(), context_.paths(), hardLinks, request.installedMods);
        mainQueue_.post([this, alive, id = request.downloadId, name = request.displayName,
                         result = std::move(result)]() mutable {
            if (!alive->load()) return;
            ModCheckView& current = state_.check;
            if (current.downloadId != id) return;
            current.running = false;
            if (!result) {
                current.error = result.error();
                if (result.error().code != ErrorCode::Cancelled) {
                    addNotice("The check of " + name + " failed: " + result.error().message, ToastKind::Error);
                }
                return;
            }
            current.report = std::move(result).value();
        });
    });
}

std::vector<mods::InstalledModPaths> AppController::installedModPaths(const std::string& titleId) const {
    std::vector<mods::InstalledModPaths> result;
    if (!games::isValidTitleId(titleId)) return result;
    auto env = installEnvironment(false);
    auto state = install::loadTitleState(env, titleId);
    if (!state) return result;
    for (const auto& mod : state->mods) {
        mods::InstalledModPaths paths{mod.downloadId, mod.name, {}};
        for (const auto& file : mod.files) paths.installPaths.push_back(file.installPath);
        result.push_back(std::move(paths));
    }
    return result;
}

void AppController::cancelCheck() {
    if (state_.check.running) checkCancel_.cancel();
}

// ---------------------------------------------------------------- installing (Phase 5)

namespace {

std::string installUnavailableReason(const AppViewState& state) {
    if (!state.systemCheck.report) return "Run the system check first.";
    const auto& feature = state.systemCheck.report->features.installation;
    if (feature.state == app::FeatureState::Available) return {};
    return feature.reason.empty() ? "Installing is not available on this console right now." : feature.reason;
}

}  // namespace

void AppController::installChecked(const std::string& id) { installDownload(id, std::nullopt); }

void AppController::testInstallChecked(const std::string& id, install::TestPlacement placement) {
    installDownload(id, placement);
}

// Applied-overlay notes for the user: what Akeno turned off instead of activating.
static std::string turnedOffText(const install::ApplyResult& applied) {
    std::string text;
    for (const auto& item : applied.turnedOff) text += " " + item;
    return text;
}

void AppController::installDownload(const std::string& id, std::optional<install::TestPlacement> test) {
    if (installing_ || state_.check.running || state_.diagnostics.running) {
        addNotice("Another mod operation is running. Wait for it to finish.", ToastKind::Warning);
        return;
    }
    if (std::string why = installUnavailableReason(state_); !why.empty()) {
        addNotice("Installing is not available: " + why, ToastKind::Error);
        return;
    }
    const downloads::DownloadInfo* item = state_.downloads.find(id);
    auto file = context_.downloads().completedFile(id);
    if (item == nullptr || !file) {
        addNotice("This download no longer exists.", ToastKind::Error);
        return;
    }
    const downloads::DownloadRequest& download = item->record.request;
    const games::GameInfo* game = nullptr;
    for (const auto& candidate : allGames_) {
        if (candidate.titleId == download.gameTitleId) game = &candidate;
    }
    if (game == nullptr) {
        addNotice("The game " + download.gameTitleId + " is not installed, so the mod cannot be installed.",
                  ToastKind::Error);
        return;
    }
    install::InstallRequest request;
    request.downloadId = id;
    request.archive = file.value();
    request.format = download.format;
    request.mod = download.mod;
    request.name = download.displayName;
    request.version = download.modVersion;
    request.titleId = download.gameTitleId;
    request.sourceType = game->sourceType;
    request.archiveRoot = download.archiveRoot;
    request.targetPrefix = download.targetPrefix;
    request.catalogueStatus = statusFromLabel(download.compatibility);
    request.catalogueInstallable = download.catalogueInstallable;
    request.pcSource = mods::isPcProvider(download.mod.providerId);
    request.curated = mods::isCuratedProvider(download.mod.providerId) && !request.pcSource;
    request.gameVersion = game->version;
    request.contentId = game->contentId;
    request.installedPkg = game->installedPkg;
    request.archiveSha256 = download.expectedSha256;
    request.test = test.has_value();
    request.placement = test.value_or(install::TestPlacement::ModsFolder);
    if (!game->installedPkg && game->sourceType == games::SourceType::Folder) request.gameFolder = game->installPath;
    const install::TitleTarget target{game->titleId, game->mounted, game->installedPkg, game->installPath};
    const bool interrupted = context_.interruptedOperation().has_value();
    auto registry = std::make_shared<const compatibility::Registry>(
        install::registryWithReports(request.titleId, testRecords(request.titleId)));

    installing_ = true;
    addNotice((request.test ? "Installing " + request.name + " as a test..." : "Installing " + request.name + "..."),
              ToastKind::Info);
    auto alive = alive_;
    tasks_.submit([this, request, target, interrupted, registry, alive] {
        auto env = installEnvironment(interrupted);
        env.registry = registry.get();
        std::string message;
        ToastKind kind = ToastKind::Success;
        auto stored = install::storeMod(request, env);
        if (!stored && stored.error().code != ErrorCode::AlreadyExists) {
            message = "Not installed: " + stored.error().message;
            kind = ToastKind::Error;
        } else if (auto applied = install::applyOverlay(target, env); !applied) {
            message = "The mod is kept but not active: " + applied.error().message;
            kind = ToastKind::Error;
        } else if (stored && stored->test) {
            const std::string folder = stored->files.empty()
                                           ? std::string()
                                           : std::filesystem::path(stored->files.front().installPath).parent_path().generic_string();
            message = request.name + " is installed as a TEST in " + folder +
                      ". Start the game and look for the mod's effect, then report the result in Installed Mods "
                      "(OPTIONS)." + turnedOffText(applied.value());
            kind = applied->turnedOff.empty() ? ToastKind::Success : ToastKind::Warning;
        } else {
            message = strings::concat(request.name, " is installed. ", applied->mods,
                                      " mod(s) are active the next time the game starts.", turnedOffText(applied.value()));
            kind = applied->turnedOff.empty() ? ToastKind::Success : ToastKind::Warning;
        }
        mainQueue_.post([this, alive, message, kind] {
            if (!alive->load()) return;
            installing_ = false;
            modSummaries_.clear();
            installedRows_.reset();
            addNotice(message, kind);
        });
    });
}

void AppController::setGameVanilla(const std::string& titleId) {
    if (installing_ || state_.check.running || state_.diagnostics.running) {
        addNotice("Another mod operation is running. Wait for it to finish.", ToastKind::Warning);
        return;
    }
    const games::GameInfo* game = nullptr;
    for (const auto& candidate : allGames_) {
        if (candidate.titleId == titleId) game = &candidate;
    }
    const install::TitleTarget target{titleId, game != nullptr && game->mounted, game != nullptr && game->installedPkg,
                                      game != nullptr ? game->installPath : std::string()};
    const bool interrupted = context_.interruptedOperation().has_value();
    installing_ = true;
    auto alive = alive_;
    tasks_.submit([this, target, interrupted, alive] {
        auto env = installEnvironment(interrupted);
        auto result = install::setVanilla(target, env);
        std::string message = result ? "Vanilla: all mods are off. The game starts unmodified next time."
                                     : "Could not switch to Vanilla: " + result.error().message;
        const ToastKind kind = result ? ToastKind::Success : ToastKind::Error;
        mainQueue_.post([this, alive, message, kind] {
            if (!alive->load()) return;
            installing_ = false;
            modSummaries_.clear();
            installedRows_.reset();
            addNotice(message, kind);
        });
    });
}

InstalledModsSummary AppController::installedMods(const std::string& titleId) {
    if (auto cached = modSummaries_.find(titleId); cached != modSummaries_.end()) return cached->second;
    InstalledModsSummary& summary = modSummaries_[titleId];
    auto env = installEnvironment(false);
    auto state = install::loadTitleState(env, titleId);
    if (!state) {
        summary.error = state.error();
        return summary;
    }
    summary.stored = state->mods.size();
    summary.enabled = state->enabledCount();
    summary.overlayActive = state->overlayActive;
    return summary;
}

// ---------------------------------------------------------------- text entry

void AppController::requestTextInput(const std::string& prompt, const std::string& initial,
                                     std::function<void(std::optional<std::string>)> done) {
    if (state_.textEntry.active) {
        finishTextInput(false);
    }
    state_.textEntry.active = true;
    state_.textEntry.systemKeyboard = context_.platform().isConsole();
    state_.textEntry.prompt = prompt;
    state_.textEntry.text = strings::sanitizeForDisplay(initial, kMaxTextInputBytes);
    textInputDone_ = std::move(done);
}

void AppController::appendTextInput(std::string_view utf8) {
    if (!state_.textEntry.active) return;
    state_.textEntry.text =
        strings::sanitizeForDisplay(state_.textEntry.text + std::string(utf8), kMaxTextInputBytes);
}

void AppController::eraseTextInput() {
    if (state_.textEntry.active) strings::popBackUtf8(state_.textEntry.text);
}

void AppController::setTextInput(std::string text) {
    if (state_.textEntry.active) state_.textEntry.text = strings::sanitizeForDisplay(text, kMaxTextInputBytes);
}

void AppController::finishTextInput(bool accepted) {
    if (!state_.textEntry.active) return;
    auto done = std::move(textInputDone_);
    textInputDone_ = nullptr;
    std::optional<std::string> result;
    if (accepted) result = std::string(strings::trim(state_.textEntry.text));
    state_.textEntry = TextEntryView{};
    if (done) done(std::move(result));
}

}  // namespace akeno::ui

namespace akeno::ui {

install::TitleTarget AppController::targetFor(const std::string& titleId) const {
    for (const auto& game : allGames_) {
        if (game.titleId == titleId) return {titleId, game.mounted, game.installedPkg, game.installPath};
    }
    return {titleId, false, false, {}};
}

void AppController::runModChange(std::function<Result<ChangeMessage>(install::InstallEnvironment&)> work) {
    if (installing_ || state_.check.running || state_.diagnostics.running) {
        addNotice("Another mod operation is running. Wait for it to finish.", ToastKind::Warning);
        return;
    }
    const bool interrupted = context_.interruptedOperation().has_value();
    installing_ = true;
    auto alive = alive_;
    tasks_.submit([this, work = std::move(work), interrupted, alive] {
        auto env = installEnvironment(interrupted);
        auto result = work(env);
        std::string message = result ? result->text : result.error().message;
        const ToastKind kind = result ? result->kind : ToastKind::Error;
        mainQueue_.post([this, alive, message, kind] {
            if (!alive->load()) return;
            installing_ = false;
            modSummaries_.clear();
            installedRows_.reset();
            addNotice(message, kind);
        });
    });
}

std::vector<InstalledModRow> AppController::listInstalledMods() {
    if (installedRows_) return *installedRows_;
    std::vector<InstalledModRow> rows;
    auto env = installEnvironment(false);
    std::error_code ec;
    std::vector<std::string> titles;
    for (std::filesystem::directory_iterator it(context_.paths().mods(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (games::isValidTitleId(name)) titles.push_back(name);
    }
    std::sort(titles.begin(), titles.end());
    for (const auto& titleId : titles) {
        auto state = install::loadTitleState(env, titleId);
        if (!state) {
            logger().warn("install", titleId + ": " + state.error().describe());
            continue;
        }
        std::string gameName = titleId;
        for (const auto& game : allGames_) {
            if (game.titleId == titleId) gameName = game.name;
        }
        for (const auto& mod : state->mods) {
            InstalledModRow row{titleId, gameName, mod.downloadId, mod.name, mod.version, mod.mod.providerId, mod.enabled,
                                state->overlayActive, mod.bytes};
            row.test = mod.test;
            row.testResult = mod.testResult;
            if (mod.test && !mod.files.empty()) {
                row.testFolder = std::filesystem::path(mod.files.front().installPath).parent_path().generic_string();
            }
            if (mod.pcSource && !mod.activationRecorded) {
                row.note = "Installed by an older Akeno without a checked path: remove it and install it again.";
            } else if (mod.activationRecorded && !mod.activationAllowed) {
                row.note = "Its check does not allow activating it.";
            } else if (mod.test && mod.testResult == install::TestResult::Crashed) {
                row.note = "You reported that it crashed the game, so it stays off.";
            }
            rows.push_back(std::move(row));
        }
    }
    installedRows_ = rows;
    return rows;
}

void AppController::setInstalledModEnabled(const std::string& titleId, const std::string& downloadId, bool enabled) {
    const install::TitleTarget target = targetFor(titleId);
    runModChange([target, downloadId, enabled](install::InstallEnvironment& env) -> Result<ChangeMessage> {
        AKENO_TRY(install::setModEnabled(env, target.titleId, downloadId, enabled));
        auto applied = install::applyOverlay(target, env);
        if (!applied) {
            // Keep the list as it was when the overlay could not follow.
            (void)install::setModEnabled(env, target.titleId, downloadId, !enabled);
            return std::move(applied).error();
        }
        if (!applied->turnedOff.empty()) return ChangeMessage{turnedOffText(applied.value()).substr(1), ToastKind::Warning};
        return ChangeMessage{enabled ? "Mod turned on. Active the next time the game starts."
                                     : "Mod turned off. The game starts without it next time."};
    });
}

void AppController::removeInstalledMod(const std::string& titleId, const std::string& downloadId) {
    const install::TitleTarget target = targetFor(titleId);
    runModChange([target, downloadId](install::InstallEnvironment& env) -> Result<ChangeMessage> {
        AKENO_TRY(install::setModEnabled(env, target.titleId, downloadId, false));
        auto applied = install::applyOverlay(target, env);
        if (!applied) return std::move(applied).error();
        AKENO_TRY(install::removeStoredMod(env, target.titleId, downloadId));
        return ChangeMessage{"Mod removed. Its files were deleted and the game's overlay rebuilt without it." +
                                 turnedOffText(applied.value()),
                             applied->turnedOff.empty() ? ToastKind::Success : ToastKind::Warning};
    });
}

void AppController::reportTestResult(const std::string& titleId, const std::string& downloadId, install::TestResult result) {
    const install::TitleTarget target = targetFor(titleId);
    runModChange([target, downloadId, result](install::InstallEnvironment& env) -> Result<ChangeMessage> {
        auto turnedOff = install::reportTestResult(env, target.titleId, downloadId, result);
        if (!turnedOff) return std::move(turnedOff).error();
        if (turnedOff.value()) {
            auto applied = install::applyOverlay(target, env);
            if (!applied) {
                return ChangeMessage{"The crash is recorded and the mod is turned off, but the overlay could not be rebuilt: " +
                                         applied.error().message + " Use Vanilla on the game's page.",
                                     ToastKind::Error};
            }
        }
        switch (result) {
            case install::TestResult::Works:
                return ChangeMessage{"Recorded: it works. Later checks for this game version use your report."};
            case install::TestResult::NoEffect:
                return ChangeMessage{"Recorded: no effect. If its files are in ~mods, open its check in Downloads and press "
                                     "SQUARE to test it directly in the package folder instead.",
                                     ToastKind::Info};
            case install::TestResult::Crashed:
                return ChangeMessage{"Recorded: crash. The mod is off, and the game starts without it next time.",
                                     ToastKind::Warning};
            case install::TestResult::Untested:
                break;
        }
        return ChangeMessage{"The result was reset.", ToastKind::Info};
    });
}

install::InstallEnvironment AppController::installEnvironment(bool interrupted) const {
    return install::InstallEnvironment{context_.fs(),
                                       context_.paths(),
                                       context_.journal(),
                                       interrupted,
                                       std::filesystem::path(std::string(install::kDefaultBackportsRoot)),
                                       limits::kStorageSafetyReserveBytes,
                                       {}};
}

std::vector<install::TestRecord> AppController::testRecords(const std::string& titleId) const {
    if (!games::isValidTitleId(titleId)) return {};
    auto env = installEnvironment(false);
    auto state = install::loadTitleState(env, titleId);
    return state ? state->testRecords() : std::vector<install::TestRecord>{};
}

}  // namespace akeno::ui
