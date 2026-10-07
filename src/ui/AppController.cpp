// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/AppController.hpp"

#include <algorithm>
#include <system_error>

#include "akeno/core/BuildInfo.hpp"
#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/database/Database.hpp"
#include "akeno/games/GameLibrary.hpp"
#include "akeno/mods/Catalog.hpp"
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
Result<downloads::DownloadRequest> buildDownloadRequest(providers::IModProvider& provider, const providers::ModRef& ref,
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
        return makeError(ErrorCode::NotFound, "The catalogue lists no file for this mod.");
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
    return request;
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
    const bool catalogueChanged = settings.catalogueUrl != state_.settings.catalogueUrl;
    AKENO_TRY(context_.saveSettings(settings));
    state_.settings = settings;
    applyFilter();
    if (catalogueChanged) {
        resetCatalogView();
    }
    return {};
}

Result<std::string> AppController::exportDiagnostics() {
    const app::SystemReport* report = state_.systemCheck.report ? &*state_.systemCheck.report : nullptr;
    auto file = context_.exportDiagnostics(report);
    if (!file) {
        return std::move(file).error();
    }
    return file->string();
}

std::vector<logging::LogRecord> AppController::recentLogs() { return context_.logRing().snapshot(); }

Status AppController::cleanInterruptedOperation() {
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

void AppController::loadCatalogGames(bool forceRefresh) {
    CatalogView& view = state_.catalog;
    auto provider = context_.catalogue();
    if (!provider) {
        view.error = context_.catalogueError().value_or(
            makeError(ErrorCode::Unavailable, "The mod catalogue address is not valid."));
        return;
    }
    if (view.loading || (view.loaded && !forceRefresh)) {
        return;
    }
    view.loading = true;
    const std::uint64_t generation = catalogGeneration_;
    tasks_.submit([this, provider, forceRefresh, generation] {
        if (forceRefresh) provider->clearCache();
        auto games = provider->listGames(nullptr);
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
    auto provider = context_.catalogue();
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
    auto provider = context_.catalogue();
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
    auto provider = context_.catalogue();
    if (!provider) {
        addNotice("The mod catalogue address is not valid.", ToastKind::Error);
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
    auto removed = context_.downloads().remove(id);
    refreshDownloads();
    return removed;
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
