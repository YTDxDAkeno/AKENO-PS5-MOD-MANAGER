// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/downloads/DownloadManager.hpp"

#include <algorithm>
#include <random>
#include <system_error>

#include "akeno/core/Strings.hpp"
#include "akeno/downloads/DownloadStore.hpp"
#include "akeno/logging/Logger.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::downloads {

namespace fs = std::filesystem;
using logging::logger;
using Clock = std::chrono::steady_clock;

namespace {

struct SinkFailure {
    bool retryable = false;
    bool discardPartial = false;
};

// Size of a plain file at `path`, or nullopt if there is none (symlinks do not count).
std::optional<std::uint64_t> plainFileSize(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status)) return std::nullopt;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    return static_cast<std::uint64_t>(size);
}

bool pathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec));
}

std::string headerValue(const network::HeaderList& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (strings::equalsIgnoreCaseAscii(key, name)) return value;
    }
    return {};
}

// Writes the response body to the partial file and checks every byte against what was asked
// for: the exact range when resuming, the expected size always.
class PartialFileSink final : public network::IBodySink {
public:
    PartialFileSink(const security::SafeFs& fs, fs::path path, std::uint64_t offset, std::uint64_t expected,
                    std::uint64_t syncInterval, std::function<void(std::uint64_t)> progress)
        : fs_(fs),
          path_(std::move(path)),
          offset_(offset),
          expected_(expected),
          syncInterval_(syncInterval),
          progress_(std::move(progress)) {}

    Status begin(long status, const network::HeaderList& headers) override {
        if (status == 206) {
            auto range = parseContentRange(headerValue(headers, "content-range"));
            if (offset_ == 0 || !range) {
                return fail(true, true, "The server's answer to a resumed download was not valid. Starting again.");
            }
            if (range->total && *range->total != expected_) {
                return fail(false, true,
                            strings::concat("The file on the server is ", strings::formatBytes(*range->total),
                                            ", not ", strings::formatBytes(expected_),
                                            " as listed. It may have been replaced; nothing was kept."));
            }
            if (range->first != offset_ || range->last + 1 != expected_) {
                return fail(true, true, "The server sent a different part of the file than requested. Starting again.");
            }
            return open(true);
        }
        if (status == 200) {
            const std::string length = headerValue(headers, "content-length");
            if (!length.empty() && length != std::to_string(expected_)) {
                return fail(false, true,
                            strings::concat("The file on the server has a different size (", length,
                                            " bytes) than listed (", expected_, " bytes). Nothing was kept."));
            }
            offset_ = 0;  // full answer: the server ignored or could not use the range
            return open(false);
        }
        if (status == 416 && offset_ > 0) {
            return fail(true, true, "The server could not continue the download. Starting again.");
        }
        if (status == 408 || status == 429 || status >= 500) {
            return fail(true, false, strings::concat("The download server is busy or failing (HTTP ", status, ")."),
                        ErrorCode::HttpStatus);
        }
        return fail(false, false,
                    status >= 400 && status < 500
                        ? strings::concat("The server refused the download (HTTP ", status, ").")
                        : strings::concat("The server gave an unexpected answer (HTTP ", status, ")."),
                    ErrorCode::HttpStatus);
    }

    Status write(std::string_view chunk) override {
        if (!file_) {
            return makeError(ErrorCode::Internal, "The download file is not open.");
        }
        if (offset_ + written_ + chunk.size() > expected_) {
            return fail(false, true, "The server sent more data than the file should have. Nothing was kept.",
                        ErrorCode::ResponseTooLarge);
        }
        auto stored = file_->write(chunk);
        if (!stored) {
            failure_ = SinkFailure{false, false};
            return stored;
        }
        written_ += chunk.size();
        sinceSync_ += chunk.size();
        if (sinceSync_ >= syncInterval_) {
            sinceSync_ = 0;
            AKENO_TRY(file_->sync());
        }
        if (progress_) progress_(offset_ + written_);
        return {};
    }

    Status finish() {
        if (!file_) return {};
        return file_->close();
    }

    std::uint64_t fileSize() const { return offset_ + written_; }
    const std::optional<SinkFailure>& failure() const { return failure_; }

private:
    Status open(bool append) {
        auto opened = fs_.openForWriting(path_, append ? security::WriteMode::Append : security::WriteMode::Truncate);
        if (!opened) {
            failure_ = SinkFailure{false, false};
            return std::move(opened).error();
        }
        file_ = std::move(opened).value();
        if (append && file_->size() != offset_) {
            file_.reset();
            return fail(true, true, "The partial download changed on disk. Starting again.");
        }
        return {};
    }

    Error fail(bool retryable, bool discard, std::string message, ErrorCode code = ErrorCode::Network) {
        failure_ = SinkFailure{retryable, discard};
        return makeError(code, std::move(message));
    }

    const security::SafeFs& fs_;
    fs::path path_;
    std::uint64_t offset_;
    std::uint64_t expected_;
    std::uint64_t syncInterval_;
    std::function<void(std::uint64_t)> progress_;
    std::unique_ptr<security::WritableFile> file_;
    std::uint64_t written_ = 0;
    std::uint64_t sinceSync_ = 0;
    std::optional<SinkFailure> failure_;
};

}  // namespace

DownloadManager::DownloadManager(network::IHttpClient& http, const security::SafeFs& fs, database::Database* db,
                                 DownloadManagerOptions options)
    : http_(http), fs_(fs), db_(db), options_(std::move(options)) {
    if (!options_.storageQuery) options_.storageQuery = security::queryStorageSpace;
}

DownloadManager::~DownloadManager() { stop(); }

bool DownloadManager::started() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_;
}

void DownloadManager::setListener(std::function<void()> listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

void DownloadManager::notify() {
    std::function<void()> listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener = listener_;
    }
    if (listener) listener();
}

DownloadManager::Entry* DownloadManager::findLocked(const std::string& id) {
    for (auto& entry : entries_) {
        if (entry.record.id == id) return &entry;
    }
    return nullptr;
}

const DownloadManager::Entry* DownloadManager::findLocked(const std::string& id) const {
    for (const auto& entry : entries_) {
        if (entry.record.id == id) return &entry;
    }
    return nullptr;
}

void DownloadManager::touchLocked(DownloadRecord& record) { record.updatedAt = strings::utcTimestamp(); }

void DownloadManager::persistLocked(const DownloadRecord& record) {
    if (db_ == nullptr) return;
    DownloadStore store(*db_);
    auto saved = store.save(record);
    if (!saved) {
        logger().warn("downloads", "could not save download " + record.id + ": " + saved.error().describe());
    }
}

void DownloadManager::removeFilesLocked(const DownloadRecord& record) {
    for (const fs::path& path : {partialPath(options_.directory, record.id),
                                 finalPath(options_.directory, record.id, record.request.format)}) {
        if (!pathExists(path)) continue;
        auto removed = fs_.removeFile(path);
        if (!removed) logger().warn("downloads", "could not remove " + path.string() + ": " + removed.error().describe());
    }
}

std::string DownloadManager::newIdLocked() {
    std::random_device::result_type entropy = 0;
    try {
        std::random_device device;
        entropy = device();
    } catch (...) {
        // Fall back to time and counter; uniqueness is checked below.
    }
    while (true) {
        const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::string id = security::sha256Hex(strings::concat(++idCounter_, ":", ticks, ":", entropy)).substr(0, 16);
        if (findLocked(id) != nullptr) continue;
        if (pathExists(partialPath(options_.directory, id))) continue;
        return id;
    }
}

Status DownloadManager::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return {};
        AKENO_TRY(fs_.createDirectories(options_.directory));

        std::vector<DownloadRecord> records;
        if (db_ != nullptr) {
            DownloadStore store(*db_);
            auto loaded = store.loadAll();
            if (!loaded) {
                logger().error("downloads", "could not read the download list: " + loaded.error().describe());
            } else {
                for (const auto& warning : loaded->warnings) logger().warn("downloads", warning);
                records = std::move(loaded->records);
            }
        }
        // Keep records added before start() (not yet stored when there is no database).
        for (auto& entry : entries_) {
            if (std::none_of(records.begin(), records.end(), [&](const DownloadRecord& r) { return r.id == entry.record.id; })) {
                records.push_back(entry.record);
            }
        }
        entries_.clear();

        for (DownloadRecord& record : records) {
            const fs::path partial = partialPath(options_.directory, record.id);
            const fs::path final = finalPath(options_.directory, record.id, record.request.format);
            bool changed = false;
            if (record.state == DownloadState::Completed) {
                if (plainFileSize(final) != record.request.expectedSize) {
                    record.state = DownloadState::Failed;
                    record.error = "The downloaded file is missing or was changed. Download it again.";
                    record.bytesDone = 0;
                    changed = true;
                }
                if (pathExists(partial)) (void)fs_.removeFile(partial);
            } else {
                if (pathExists(final)) {
                    // Interrupted between verification and the database update: check it again.
                    if (!pathExists(partial) && plainFileSize(final)) {
                        (void)fs_.rename(final, partial);
                    } else {
                        (void)fs_.removeFile(final);
                    }
                }
                auto size = plainFileSize(partial);
                if (size && *size > record.request.expectedSize) {
                    (void)fs_.removeFile(partial);
                    size.reset();
                }
                record.bytesDone = size.value_or(0);
                if (record.state == DownloadState::Downloading || record.state == DownloadState::Verifying) {
                    record.state = DownloadState::Queued;  // interrupted: continue automatically
                    changed = true;
                }
            }
            if (changed) {
                touchLocked(record);
                persistLocked(record);
            }
            Entry entry;
            entry.record = std::move(record);
            entries_.push_back(std::move(entry));
        }

        // Files Akeno created that no record refers to (for example after a crash during
        // removal). Only Akeno's own file names are considered; anything else is left alone.
        std::error_code ec;
        for (fs::directory_iterator it(options_.directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::string id;
            const std::string name = it->path().filename().string();
            if (!isDownloadFileName(name, &id) || findLocked(id) != nullptr) continue;
            auto removed = fs_.removeFile(it->path());
            logger().info("downloads", "removed an orphaned download file " + name +
                                           (removed ? std::string() : " (failed: " + removed.error().describe() + ")"));
        }

        started_ = true;
        stopping_ = false;
        worker_ = std::thread([this] { run(); });
        logger().info("downloads", strings::concat("download engine started, ", entries_.size(), " downloads"));
    }
    notify();
    return {};
}

void DownloadManager::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        stopping_ = true;
        if (!activeId_.empty()) {
            interrupt_ = Interrupt::Stop;
            activeCancel_.cancel();
        }
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

Result<DownloadManager::Enqueued> DownloadManager::enqueue(DownloadRequest request) {
    AKENO_TRY(validateRequest(request));
    request.displayName = strings::sanitizeForDisplay(request.displayName, limits::kMaxDisplayStringBytes);
    Enqueued result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            const DownloadRequest& existing = entry.record.request;
            if (existing.mod == request.mod && existing.modVersion == request.modVersion &&
                existing.expectedSha256 == request.expectedSha256) {
                if (entry.record.state == DownloadState::Failed || entry.record.state == DownloadState::Paused) {
                    entry.record.state = DownloadState::Queued;
                    entry.record.error.clear();
                    entry.record.attempts = 0;
                    touchLocked(entry.record);
                    persistLocked(entry.record);
                }
                result.id = entry.record.id;
                result.alreadyPresent = true;
                break;
            }
        }
        if (!result.alreadyPresent) {
            Entry entry;
            entry.record.id = newIdLocked();
            entry.record.request = std::move(request);
            entry.record.state = DownloadState::Queued;
            entry.record.createdAt = strings::utcTimestamp();
            entry.record.updatedAt = entry.record.createdAt;
            persistLocked(entry.record);
            logger().info("downloads", "queued " + entry.record.id + ": " + entry.record.request.displayName + " " +
                                           entry.record.request.modVersion);
            result.id = entry.record.id;
            entries_.push_back(std::move(entry));
        }
    }
    wake_.notify_all();
    notify();
    return result;
}

Status DownloadManager::pause(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = findLocked(id);
        if (entry == nullptr) return makeError(ErrorCode::NotFound, "This download no longer exists.", id);
        if (id == activeId_) {
            interrupt_ = Interrupt::Pause;
            activeCancel_.cancel();
        } else if (entry->record.state == DownloadState::Queued) {
            entry->record.state = DownloadState::Paused;
            touchLocked(entry->record);
            persistLocked(entry->record);
        } else if (entry->record.state != DownloadState::Paused) {
            return makeError(ErrorCode::InvalidArgument, "This download cannot be paused now.");
        }
    }
    wake_.notify_all();
    notify();
    return {};
}

Status DownloadManager::resume(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = findLocked(id);
        if (entry == nullptr) return makeError(ErrorCode::NotFound, "This download no longer exists.", id);
        DownloadRecord& record = entry->record;
        if (record.state == DownloadState::Completed) {
            return makeError(ErrorCode::InvalidArgument, "This download is already complete.");
        }
        if (record.state == DownloadState::Paused || record.state == DownloadState::Failed) {
            record.state = DownloadState::Queued;
            record.error.clear();
            record.attempts = 0;
            touchLocked(record);
            persistLocked(record);
        }
    }
    wake_.notify_all();
    notify();
    return {};
}

Status DownloadManager::remove(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = findLocked(id);
        if (entry == nullptr) return makeError(ErrorCode::NotFound, "This download no longer exists.", id);
        if (id == activeId_) {
            interrupt_ = Interrupt::Remove;  // the worker deletes it when the transfer has stopped
            activeCancel_.cancel();
        } else {
            removeFilesLocked(entry->record);
            if (db_ != nullptr) {
                DownloadStore store(*db_);
                auto removed = store.remove(id);
                if (!removed) logger().warn("downloads", "could not delete record " + id + ": " + removed.error().describe());
            }
            logger().info("downloads", "removed " + id);
            entries_.erase(entries_.begin() + (entry - entries_.data()));
        }
    }
    wake_.notify_all();
    notify();
    return {};
}

std::vector<DownloadInfo> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DownloadInfo> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(DownloadInfo{entry.record, entry.progress});
    return out;
}

std::optional<DownloadInfo> DownloadManager::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* entry = findLocked(id);
    if (entry == nullptr) return std::nullopt;
    return DownloadInfo{entry->record, entry->progress};
}

std::optional<DownloadInfo> DownloadManager::findForMod(const providers::ModRef& mod, const std::string& version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->record.request.mod == mod && it->record.request.modVersion == version) {
            return DownloadInfo{it->record, it->progress};
        }
    }
    return std::nullopt;
}

Result<fs::path> DownloadManager::completedFile(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* entry = findLocked(id);
    if (entry == nullptr || entry->record.state != DownloadState::Completed) {
        return makeError(ErrorCode::NotFound, "This download is not complete.", id);
    }
    fs::path path = finalPath(options_.directory, id, entry->record.request.format);
    if (plainFileSize(path) != entry->record.request.expectedSize) {
        return makeError(ErrorCode::NotFound, "The downloaded file is missing or was changed.", path.string());
    }
    return path;
}

void DownloadManager::reportProgress(const std::string& id, std::uint64_t bytes, bool verifying) {
    const auto now = Clock::now();
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = findLocked(id);
        if (entry == nullptr) return;
        if (verifying) {
            entry->progress.verifiedBytes = bytes;
        } else {
            entry->record.bytesDone = bytes;
            if (bytes < entry->sampleBytes) {  // started again from zero
                entry->sampleBytes = bytes;
                entry->sampleTime = now;
            }
            const double elapsed = std::chrono::duration<double>(now - entry->sampleTime).count();
            if (elapsed >= 0.5) {
                const double rate = static_cast<double>(bytes - entry->sampleBytes) / elapsed;
                double& speed = entry->progress.bytesPerSecond;
                speed = speed <= 0.0 ? rate : 0.7 * speed + 0.3 * rate;
                entry->sampleBytes = bytes;
                entry->sampleTime = now;
                if (speed > 1.0) {
                    entry->progress.secondsLeft =
                        static_cast<double>(entry->record.request.expectedSize - bytes) / speed;
                }
            }
        }
        if (now - lastNotify_ >= std::chrono::milliseconds(250)) {
            lastNotify_ = now;
            shouldNotify = true;
        }
    }
    if (shouldNotify) notify();
}

bool DownloadManager::waitInterruptible(std::chrono::milliseconds delay, const CancellationToken& cancel) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, delay, [&] { return stopping_ || cancel.cancelled(); });
    return stopping_ || cancel.cancelled();
}

std::optional<DownloadManager::Failure> DownloadManager::transferOnce(const DownloadRecord& job,
                                                                     const CancellationToken& cancel) {
    const DownloadRequest& request = job.request;
    const fs::path partial = partialPath(options_.directory, job.id);
    std::uint64_t offset = 0;
    if (auto size = plainFileSize(partial)) {
        offset = *size;
    } else if (pathExists(partial)) {
        (void)fs_.removeFile(partial);  // not a plain file: never write through it
    }
    if (offset > request.expectedSize) {
        (void)fs_.removeFile(partial);
        offset = 0;
    }
    if (offset == request.expectedSize) {
        return std::nullopt;  // already complete; verification decides
    }

    // Free space: the rest of the file plus the reserve that always stays free.
    auto space = options_.storageQuery(options_.directory);
    if (!space) {
        return Failure{makeError(ErrorCode::IoError, "Could not check the free space, so nothing was downloaded.",
                                 space.error().describe()),
                       false, false};
    }
    const std::uint64_t needed = request.expectedSize - offset;
    if (space->availableBytes < needed + options_.storageReserve) {
        return Failure{makeError(ErrorCode::NoSpace,
                                 strings::concat("Not enough free space: this download needs ",
                                                 strings::formatBytes(needed), " and Akeno always keeps ",
                                                 strings::formatBytes(options_.storageReserve), " free. Available: ",
                                                 strings::formatBytes(space->availableBytes), ".")),
                       false, false};
    }

    network::HttpRequest http;
    http.method = network::HttpMethod::Get;
    http.url = request.url;
    http.headers = {{"Accept", "*/*"}};
    if (offset > 0) http.headers.emplace_back("Range", strings::concat("bytes=", offset, "-"));
    http.maxResponseBytes = static_cast<std::size_t>(request.expectedSize);
    http.connectTimeoutMs = options_.connectTimeoutMs;
    http.totalTimeoutMs = options_.totalTimeoutMs;
    http.acceptCompressed = false;

    const std::string id = job.id;
    PartialFileSink sink(fs_, partial, offset, request.expectedSize, options_.syncInterval,
                         [this, id](std::uint64_t bytes) { reportProgress(id, bytes, false); });
    auto response = http_.stream(http, sink, &cancel);
    if (!response) {
        Error error = std::move(response).error();
        (void)sink.finish();
        if (sink.failure()) {
            return Failure{std::move(error), sink.failure()->retryable, sink.failure()->discardPartial};
        }
        const bool retryable =
            error.code == ErrorCode::Network || error.code == ErrorCode::Timeout || error.code == ErrorCode::Unavailable;
        return Failure{std::move(error), retryable, false};
    }
    auto finished = sink.finish();
    if (!finished) {
        return Failure{std::move(finished).error(), false, false};
    }
    if (sink.fileSize() != request.expectedSize) {
        return Failure{makeError(ErrorCode::Network, "The connection ended before the download was complete."), true,
                       false};
    }
    return std::nullopt;
}

DownloadManager::Outcome DownloadManager::process(const DownloadRecord& job, const CancellationToken& cancel,
                                                  std::string& failure) {
    const fs::path partial = partialPath(options_.directory, job.id);
    for (std::size_t attempt = 0;; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (Entry* entry = findLocked(job.id)) {
                entry->record.attempts = static_cast<int>(attempt) + 1;
                entry->progress.retryInSeconds.reset();
            }
        }
        auto problem = transferOnce(job, cancel);
        if (cancel.cancelled()) return Outcome::Interrupted;
        if (!problem) break;
        if (problem->discardPartial && pathExists(partial)) (void)fs_.removeFile(partial);
        logger().warn("downloads", job.id + ": " + problem->error.describe());
        if (!problem->retryable || attempt >= options_.retryDelays.size()) {
            failure = problem->error.message;
            return Outcome::Failed;
        }
        const auto delay = options_.retryDelays[attempt];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (Entry* entry = findLocked(job.id)) {
                entry->progress.retryInSeconds = std::chrono::duration<double>(delay).count();
                entry->record.error = problem->error.message;
            }
        }
        notify();
        if (waitInterruptible(delay, cancel)) return Outcome::Interrupted;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Entry* entry = findLocked(job.id)) {
            entry->record.state = DownloadState::Verifying;
            entry->record.bytesDone = job.request.expectedSize;
            entry->record.error.clear();
            entry->progress = DownloadProgress{};
            touchLocked(entry->record);
            persistLocked(entry->record);
        }
    }
    notify();
    const std::string id = job.id;
    auto digest = security::sha256File(partial, &cancel, [this, id](std::uint64_t n) { reportProgress(id, n, true); });
    if (cancel.cancelled()) return Outcome::Interrupted;
    if (!digest) {
        failure = "Could not read the downloaded file to verify it: " + digest.error().message;
        return Outcome::Failed;
    }
    if (digest.value() != job.request.expectedSha256) {
        logger().error("downloads", job.id + ": SHA-256 mismatch, expected " + job.request.expectedSha256 + ", got " +
                                        digest.value());
        (void)fs_.removeFile(partial);
        failure = "The downloaded file does not match its SHA-256 checksum, so it was deleted. Try again; if this "
                  "repeats, the catalogue entry is wrong.";
        return Outcome::Failed;
    }
    auto renamed = fs_.rename(partial, finalPath(options_.directory, job.id, job.request.format));
    if (!renamed) {
        failure = "The verified file could not be stored: " + renamed.error().message;
        return Outcome::Failed;
    }
    return Outcome::Completed;
}

void DownloadManager::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        Entry* next = nullptr;
        for (auto& entry : entries_) {
            if (entry.record.state == DownloadState::Queued) {
                next = &entry;
                break;
            }
        }
        if (next == nullptr) {
            wake_.wait(lock);
            continue;
        }
        DownloadRecord& record = next->record;
        record.state = DownloadState::Downloading;
        record.error.clear();
        record.attempts = 0;
        next->progress = DownloadProgress{};
        next->sampleTime = Clock::now();
        next->sampleBytes = record.bytesDone;
        touchLocked(record);
        persistLocked(record);
        activeId_ = record.id;
        interrupt_ = Interrupt::None;
        activeCancel_ = CancellationToken{};
        const DownloadRecord job = record;
        const CancellationToken cancel = activeCancel_;
        logger().info("downloads", "starting " + job.id + " (" + job.request.displayName + ")");

        lock.unlock();
        notify();
        std::string failure;
        const Outcome outcome = process(job, cancel, failure);
        lock.lock();

        const Interrupt interrupt = interrupt_;
        activeId_.clear();
        interrupt_ = Interrupt::None;
        if (Entry* entry = findLocked(job.id)) {
            DownloadRecord& done = entry->record;
            entry->progress = DownloadProgress{};
            const auto partialSize = plainFileSize(partialPath(options_.directory, done.id));
            if (interrupt == Interrupt::Remove) {
                removeFilesLocked(done);
                if (db_ != nullptr) (void)DownloadStore(*db_).remove(done.id);
                logger().info("downloads", "removed " + done.id);
                entries_.erase(entries_.begin() + (entry - entries_.data()));
            } else if (outcome == Outcome::Completed) {
                done.state = DownloadState::Completed;
                done.bytesDone = done.request.expectedSize;
                done.completedAt = strings::utcTimestamp();
                done.error.clear();
                touchLocked(done);
                persistLocked(done);
                logger().info("downloads", "completed and verified " + done.id);
            } else if (outcome == Outcome::Interrupted || interrupt != Interrupt::None) {
                done.state = interrupt == Interrupt::Stop ? DownloadState::Queued : DownloadState::Paused;
                done.bytesDone = partialSize.value_or(0);
                done.error.clear();
                touchLocked(done);
                persistLocked(done);
                logger().info("downloads", strings::concat(interrupt == Interrupt::Stop ? "interrupted " : "paused ",
                                                           done.id, " at ", done.bytesDone, " bytes"));
            } else {
                done.state = DownloadState::Failed;
                done.error = failure;
                done.bytesDone = partialSize.value_or(0);
                touchLocked(done);
                persistLocked(done);
                logger().warn("downloads", "failed " + done.id + ": " + failure);
            }
        }
        lock.unlock();
        notify();
        lock.lock();
    }
}

}  // namespace akeno::downloads
