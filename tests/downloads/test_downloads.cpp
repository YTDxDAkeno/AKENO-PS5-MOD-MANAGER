// SPDX-License-Identifier: GPL-3.0-or-later
// The download engine against the real libcurl client and a loopback server.
#include "Doctest.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "LocalHttpServer.hpp"
#include "TestSupport.hpp"
#include "akeno/database/Migrations.hpp"
#include "akeno/downloads/DownloadManager.hpp"
#include "akeno/downloads/DownloadStore.hpp"
#include "akeno/network/CurlHttpClient.hpp"
#include "akeno/security/Sha256.hpp"

using namespace akeno;
using namespace akeno::downloads;
namespace fs = std::filesystem;

namespace {

network::CurlGlobal& curlGlobal() {
    static network::CurlGlobal global;
    return global;
}

std::string makePayload(std::size_t size) {
    std::string data(size, '\0');
    std::uint32_t x = 2463534242u;
    for (auto& c : data) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x & 0xFF);
    }
    return data;
}

std::unique_ptr<database::Database> migratedDatabase(const fs::path& file) {
    auto db = database::Database::open(file);
    REQUIRE(db.ok());
    std::lock_guard<std::mutex> lock(db.value()->mutex());
    REQUIRE(database::migrate(*db.value(), database::builtinMigrations(), nullptr).ok());
    return std::move(db).value();
}

// Serves `payload` at /mod.zip. Honours "Range: bytes=N-" unless told not to.
struct Server {
    std::string payload;
    std::atomic<bool> honourRange{true};
    std::atomic<int> failuresBefore503{0};   // answer 503 this many times first
    std::atomic<int> dropOnce{0};            // drop the connection after this many bytes, once
    std::atomic<int> chunkDelayMs{0};
    std::atomic<int> status{200};
    std::string lengthOverride;
    test::LocalHttpServer http{[this](const test::ReceivedRequest& request) { return answer(request); }};

    test::CannedResponse answer(const test::ReceivedRequest& request) {
        test::CannedResponse response;
        response.contentType = "application/zip";
        if (failuresBefore503 > 0) {
            --failuresBefore503;
            response.status = 503;
            response.body = "busy";
            return response;
        }
        if (status != 200) {
            response.status = status;
            response.body = "no";
            return response;
        }
        std::size_t offset = 0;
        for (const auto& [name, value] : request.headers) {
            if ((name == "Range" || name == "range") && honourRange && value.rfind("bytes=", 0) == 0) {
                offset = std::stoull(value.substr(6));
            }
        }
        if (offset > 0 && offset < payload.size()) {
            response.status = 206;
            response.headers = {{"Content-Range", "bytes " + std::to_string(offset) + "-" +
                                                      std::to_string(payload.size() - 1) + "/" +
                                                      std::to_string(payload.size())}};
            response.body = payload.substr(offset);
        } else {
            response.body = payload;
        }
        response.contentLengthOverride = lengthOverride;
        if (dropOnce > 0) {
            response.closeAfterBytes = static_cast<std::size_t>(dropOnce.exchange(0));
            response.contentLengthOverride = std::to_string(response.body.size());
        }
        if (chunkDelayMs > 0) {
            response.chunkSize = 8 * 1024;
            response.chunkDelayMs = chunkDelayMs;
        }
        return response;
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(http.port()) + "/mod.zip"; }

    int rangeRequests() const {
        int count = 0;
        for (const auto& request : http.received()) {
            for (const auto& [name, value] : request.headers) {
                if (name == "Range" || name == "range") ++count;
            }
        }
        return count;
    }
};

struct Fixture {
    test::TempDir dir;
    fs::path root = dir.path() / "data";
    fs::path downloads = root / "downloads";
    std::unique_ptr<security::SafeFs> fs;
    std::unique_ptr<database::Database> db;
    network::CurlHttpClient http{{}};
    Server server;
    std::atomic<std::uint64_t> available{100ull * 1024 * 1024 * 1024};

    explicit Fixture(std::size_t payloadSize = 300 * 1024) {
        REQUIRE(curlGlobal().ok());
        fs::create_directories(downloads);
        fs = std::make_unique<security::SafeFs>(security::WriteGuard::create({root}).value());
        db = migratedDatabase(root / "akeno.sqlite");
        server.payload = makePayload(payloadSize);
    }

    DownloadManagerOptions options() {
        DownloadManagerOptions o;
        o.directory = downloads;
        o.retryDelays = {std::chrono::milliseconds(10), std::chrono::milliseconds(10), std::chrono::milliseconds(10)};
        o.connectTimeoutMs = 2000;
        o.totalTimeoutMs = 20000;
        o.syncInterval = 64 * 1024;
        o.storageQuery = [this](const fs::path&) -> Result<security::StorageSpace> {
            return security::StorageSpace{available.load() * 2, available.load()};
        };
        return o;
    }

    std::unique_ptr<DownloadManager> manager() {
        return std::make_unique<DownloadManager>(http, *fs, db.get(), options());
    }

    DownloadRequest request(std::string sha = {}) {
        DownloadRequest r;
        r.mod = {"akeno-catalogue", "example-blade/crimson-outfit"};
        r.displayName = "Crimson Outfit Recolour";
        r.modVersion = "1.2.0";
        r.gameTitleId = "PPSA90001";
        r.gameVersion = "01.011.000";
        r.compatibility = "VERIFIED";
        r.url = server.url();
        r.expectedSize = server.payload.size();
        r.expectedSha256 = sha.empty() ? security::sha256Hex(server.payload) : sha;
        r.format = mods::ArchiveFormat::Zip;
        return r;
    }

    std::vector<std::string> files() const {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(downloads)) names.push_back(entry.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }
};

template <typename Predicate>
std::optional<DownloadInfo> waitFor(DownloadManager& manager, const std::string& id, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        auto info = manager.find(id);
        if (info && predicate(*info)) return info;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return manager.find(id);
}

bool settled(const DownloadInfo& info) {
    return info.record.state == DownloadState::Completed || info.record.state == DownloadState::Failed ||
           info.record.state == DownloadState::Paused;
}

std::string readFile(const fs::path& path) { return security::readFileBounded(path, 64 * 1024 * 1024).value(); }

}  // namespace

TEST_CASE("download requests are validated before anything is written") {
    Fixture f;
    DownloadRequest good = f.request();
    CHECK(validateRequest(good).ok());

    DownloadRequest insecure = good;
    insecure.url = "http://example.com/mod.zip";
    CHECK(validateRequest(insecure).error().code == ErrorCode::SafetyViolation);
    DownloadRequest noHash = good;
    noHash.expectedSha256.clear();
    CHECK_FALSE(validateRequest(noHash).ok());
    DownloadRequest upperHash = good;
    upperHash.expectedSha256 = std::string(64, 'A');
    CHECK_FALSE(validateRequest(upperHash).ok());
    DownloadRequest noSize = good;
    noSize.expectedSize = 0;
    CHECK_FALSE(validateRequest(noSize).ok());
    DownloadRequest huge = good;
    huge.expectedSize = mods::kMaxDownloadSize + 1;
    CHECK_FALSE(validateRequest(huge).ok());
    DownloadRequest unknownFormat = good;
    unknownFormat.format = mods::ArchiveFormat::Unknown;
    CHECK(validateRequest(unknownFormat).error().code == ErrorCode::Unsupported);

    auto manager = f.manager();
    CHECK_FALSE(manager->enqueue(insecure).ok());
    CHECK(manager->snapshot().empty());
    CHECK(f.files().empty());
}

TEST_CASE("file names come from Akeno, never from the server") {
    CHECK(isValidDownloadId("0123456789abcdef"));
    CHECK_FALSE(isValidDownloadId("0123456789ABCDEF"));
    CHECK_FALSE(isValidDownloadId("../../../etc/pwd"));
    CHECK(partialPath("/d", "0123456789abcdef") == fs::path("/d/0123456789abcdef.partial"));
    CHECK(finalPath("/d", "0123456789abcdef", mods::ArchiveFormat::TarGz) == fs::path("/d/0123456789abcdef.tar.gz"));
    std::string id;
    CHECK(isDownloadFileName("0123456789abcdef.partial", &id));
    CHECK(id == "0123456789abcdef");
    CHECK(isDownloadFileName("0123456789abcdef.7z"));
    CHECK_FALSE(isDownloadFileName("0123456789abcdef.exe"));
    CHECK_FALSE(isDownloadFileName("notes.txt"));
    CHECK_FALSE(isDownloadFileName("0123456789abcdef"));
}

TEST_CASE("Content-Range parsing") {
    auto range = parseContentRange("bytes 100-999/1000");
    REQUIRE(range);
    CHECK(range->first == 100);
    CHECK(range->last == 999);
    CHECK(range->total == std::optional<std::uint64_t>(1000));
    CHECK(parseContentRange("bytes 0-0/*"));
    CHECK_FALSE(parseContentRange("bytes 100-999/999"));
    CHECK_FALSE(parseContentRange("bytes 5-1/10"));
    CHECK_FALSE(parseContentRange("items 0-1/2"));
    CHECK_FALSE(parseContentRange("bytes */1000"));
    CHECK_FALSE(parseContentRange("bytes -1-5/10"));
}

TEST_CASE("a download is stored, verified and renamed") {
    Fixture f;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    CHECK_FALSE(queued->alreadyPresent);
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Completed);
    CHECK(info->record.error.empty());
    CHECK(info->record.bytesDone == f.server.payload.size());

    auto file = manager->completedFile(queued->id);
    REQUIRE(file.ok());
    CHECK(file.value() == f.downloads / (queued->id + ".zip"));
    CHECK(readFile(file.value()) == f.server.payload);
    CHECK(f.files() == std::vector<std::string>{queued->id + ".zip"});

    // The same file again is not downloaded twice.
    auto again = manager->enqueue(f.request());
    REQUIRE(again.ok());
    CHECK(again->alreadyPresent);
    CHECK(again->id == queued->id);

    // The record survives in the database.
    auto stored = DownloadStore(*f.db).loadAll();
    REQUIRE(stored.ok());
    REQUIRE(stored->records.size() == 1);
    CHECK(stored->records[0].state == DownloadState::Completed);
    CHECK(stored->records[0].request.expectedSha256 == security::sha256Hex(f.server.payload));
}

TEST_CASE("an interrupted download resumes with a range request") {
    Fixture f;
    auto manager = f.manager();
    auto queued = manager->enqueue(f.request());  // not started yet
    REQUIRE(queued.ok());
    {
        std::ofstream partial(f.downloads / (queued->id + ".partial"), std::ios::binary);
        partial << f.server.payload.substr(0, 100 * 1024);
    }
    REQUIRE(manager->start().ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Completed);
    CHECK(f.server.rangeRequests() == 1);
    CHECK(f.server.http.received().size() == 1);
    CHECK(readFile(manager->completedFile(queued->id).value()) == f.server.payload);
}

TEST_CASE("a server that ignores the range sends the whole file again") {
    Fixture f;
    f.server.honourRange = false;
    auto manager = f.manager();
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    {
        std::ofstream partial(f.downloads / (queued->id + ".partial"), std::ios::binary);
        partial << f.server.payload.substr(0, 50 * 1024);
    }
    REQUIRE(manager->start().ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Completed);
    CHECK(readFile(manager->completedFile(queued->id).value()) == f.server.payload);
}

TEST_CASE("a checksum mismatch fails and deletes the file") {
    Fixture f;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request(security::sha256Hex("something else")));
    REQUIRE(queued.ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Failed);
    CHECK(info->record.error.find("does not match its SHA-256") != std::string::npos);
    CHECK(f.files().empty());
    CHECK_FALSE(manager->completedFile(queued->id).ok());
}

TEST_CASE("a different file size on the server is refused without retrying") {
    Fixture f;
    f.server.lengthOverride = std::to_string(f.server.payload.size() + 10);
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Failed);
    CHECK(info->record.error.find("different size") != std::string::npos);
    CHECK(f.server.http.received().size() == 1);
    CHECK(f.files().empty());
}

TEST_CASE("more data than expected is refused") {
    Fixture f;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    DownloadRequest request = f.request();
    request.expectedSize = f.server.payload.size() - 1000;
    auto queued = manager->enqueue(request);
    REQUIRE(queued.ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Failed);
    CHECK(f.files().empty());
}

TEST_CASE("HTTP errors: permanent ones fail at once, temporary ones are retried") {
    SUBCASE("404") {
        Fixture f;
        f.server.status = 404;
        auto manager = f.manager();
        REQUIRE(manager->start().ok());
        auto queued = manager->enqueue(f.request());
        REQUIRE(queued.ok());
        auto info = waitFor(*manager, queued->id, settled);
        REQUIRE(info);
        CHECK(info->record.state == DownloadState::Failed);
        CHECK(info->record.error == "The server refused the download (HTTP 404).");
        CHECK(f.server.http.received().size() == 1);
    }
    SUBCASE("503 twice, then success") {
        Fixture f;
        f.server.failuresBefore503 = 2;
        auto manager = f.manager();
        REQUIRE(manager->start().ok());
        auto queued = manager->enqueue(f.request());
        REQUIRE(queued.ok());
        auto info = waitFor(*manager, queued->id, settled);
        REQUIRE(info);
        CHECK(info->record.state == DownloadState::Completed);
        CHECK(f.server.http.received().size() == 3);
    }
    SUBCASE("503 forever") {
        Fixture f;
        f.server.failuresBefore503 = 100;
        auto manager = f.manager();
        REQUIRE(manager->start().ok());
        auto queued = manager->enqueue(f.request());
        REQUIRE(queued.ok());
        auto info = waitFor(*manager, queued->id, settled);
        REQUIRE(info);
        CHECK(info->record.state == DownloadState::Failed);
        CHECK(info->record.error.find("HTTP 503") != std::string::npos);
        CHECK(f.server.http.received().size() == 4);  // first attempt + three retries
    }
}

TEST_CASE("a dropped connection is resumed automatically") {
    Fixture f;
    f.server.dropOnce = 120 * 1024;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Completed);
    CHECK(f.server.http.received().size() == 2);
    CHECK(f.server.rangeRequests() == 1);
    CHECK(readFile(manager->completedFile(queued->id).value()) == f.server.payload);
}

TEST_CASE("nothing is downloaded without enough free space") {
    Fixture f;
    f.available = limits::kStorageSafetyReserveBytes + 1000;  // the reserve must stay free
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    auto info = waitFor(*manager, queued->id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Failed);
    CHECK(info->record.error.find("Not enough free space") != std::string::npos);
    CHECK(f.server.http.received().empty());
    CHECK(f.files().empty());
}

TEST_CASE("pause keeps the partial file and resume finishes it") {
    Fixture f(512 * 1024);
    f.server.chunkDelayMs = 10;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    REQUIRE(waitFor(*manager, queued->id, [](const DownloadInfo& i) { return i.record.bytesDone > 32 * 1024; }));
    REQUIRE(manager->pause(queued->id).ok());
    auto paused = waitFor(*manager, queued->id, settled);
    REQUIRE(paused);
    CHECK(paused->record.state == DownloadState::Paused);
    CHECK(paused->record.bytesDone > 0);
    CHECK(paused->record.bytesDone < f.server.payload.size());
    CHECK(fs::file_size(f.downloads / (queued->id + ".partial")) == paused->record.bytesDone);

    f.server.chunkDelayMs = 0;
    REQUIRE(manager->resume(queued->id).ok());
    auto done = waitFor(*manager, queued->id, [](const DownloadInfo& i) { return i.record.state == DownloadState::Completed || i.record.state == DownloadState::Failed; });
    REQUIRE(done);
    CHECK(done->record.state == DownloadState::Completed);
    CHECK(f.server.rangeRequests() == 1);
    CHECK(readFile(manager->completedFile(queued->id).value()) == f.server.payload);
}

TEST_CASE("removing an active download deletes everything") {
    Fixture f(512 * 1024);
    f.server.chunkDelayMs = 10;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    REQUIRE(waitFor(*manager, queued->id, [](const DownloadInfo& i) { return i.record.bytesDone > 0; }));
    REQUIRE(manager->remove(queued->id).ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (manager->find(queued->id) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(manager->find(queued->id));
    CHECK(f.files().empty());
    CHECK(DownloadStore(*f.db).loadAll()->records.empty());
}

TEST_CASE("downloads continue after a restart") {
    Fixture f(512 * 1024);
    f.server.chunkDelayMs = 10;
    std::string id;
    {
        auto manager = f.manager();
        REQUIRE(manager->start().ok());
        auto queued = manager->enqueue(f.request());
        REQUIRE(queued.ok());
        id = queued->id;
        REQUIRE(waitFor(*manager, id, [](const DownloadInfo& i) { return i.record.bytesDone > 32 * 1024; }));
        manager->stop();
        auto stopped = manager->find(id);
        REQUIRE(stopped);
        CHECK(stopped->record.state == DownloadState::Queued);
    }
    auto stored = DownloadStore(*f.db).loadAll();
    REQUIRE(stored.ok());
    REQUIRE(stored->records.size() == 1);
    CHECK(stored->records[0].state == DownloadState::Queued);

    f.server.chunkDelayMs = 0;
    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    auto info = waitFor(*manager, id, settled);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Completed);
    CHECK(f.server.rangeRequests() == 1);
    CHECK(readFile(manager->completedFile(id).value()) == f.server.payload);
}

TEST_CASE("startup removes only Akeno's own orphaned files and checks completed ones") {
    Fixture f;
    std::string id;
    {
        auto manager = f.manager();
        REQUIRE(manager->start().ok());
        auto queued = manager->enqueue(f.request());
        REQUIRE(queued.ok());
        id = queued->id;
        REQUIRE(waitFor(*manager, id, settled)->record.state == DownloadState::Completed);
    }
    test::writeText(f.downloads / "0123456789abcdef.partial", "orphan");
    test::writeText(f.downloads / "fedcba9876543210.zip", "orphan");
    test::writeText(f.downloads / "keep-me.txt", "not ours");
    fs::remove(f.downloads / (id + ".zip"));  // the completed file disappeared

    auto manager = f.manager();
    REQUIRE(manager->start().ok());
    CHECK(f.files() == std::vector<std::string>{"keep-me.txt"});
    auto info = manager->find(id);
    REQUIRE(info);
    CHECK(info->record.state == DownloadState::Failed);
    CHECK(info->record.error.find("missing") != std::string::npos);

    // Retrying downloads it again.
    REQUIRE(manager->resume(id).ok());
    CHECK(waitFor(*manager, id, [](const DownloadInfo& i) { return i.record.state == DownloadState::Completed; })
              ->record.state == DownloadState::Completed);
}

TEST_CASE("invalid stored rows are ignored, not trusted") {
    Fixture f;
    REQUIRE(f.db->exec("INSERT INTO downloads(id, provider_id, mod_id, name, url, expected_size, expected_sha256, "
                       "format, state, created_at, updated_at) VALUES ('../../evil', 'p', 'm', 'n', "
                       "'https://example.com/a.zip', 10, '" + std::string(64, 'a') +
                       "', 'zip', 'queued', 'x', 'x');")
                .ok());
    REQUIRE(f.db->exec("INSERT INTO downloads(id, provider_id, mod_id, name, url, expected_size, expected_sha256, "
                       "format, state, created_at, updated_at) VALUES ('0123456789abcdef', 'p', 'm', 'n', "
                       "'http://example.com/a.zip', 10, '" + std::string(64, 'a') +
                       "', 'zip', 'queued', 'x', 'x');")
                .ok());
    auto loaded = DownloadStore(*f.db).loadAll();
    REQUIRE(loaded.ok());
    CHECK(loaded->records.empty());
    CHECK(loaded->warnings.size() == 2);
}

TEST_CASE("pause and resume of queued downloads, and errors for unknown ids") {
    Fixture f;
    auto manager = f.manager();
    auto queued = manager->enqueue(f.request());
    REQUIRE(queued.ok());
    REQUIRE(manager->pause(queued->id).ok());
    CHECK(manager->find(queued->id)->record.state == DownloadState::Paused);
    REQUIRE(manager->resume(queued->id).ok());
    CHECK(manager->find(queued->id)->record.state == DownloadState::Queued);
    CHECK(manager->pause("ffffffffffffffff").error().code == ErrorCode::NotFound);
    CHECK(manager->remove("ffffffffffffffff").error().code == ErrorCode::NotFound);
    REQUIRE(manager->remove(queued->id).ok());
    CHECK(manager->snapshot().empty());
}
