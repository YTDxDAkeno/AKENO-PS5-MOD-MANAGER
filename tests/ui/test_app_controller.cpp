// SPDX-License-Identifier: GPL-3.0-or-later
// The controller against real services: AppContext, libcurl, worker threads and a loopback
// server that serves the catalogue fixtures.
#include "Doctest.hpp"

#include <chrono>
#include <map>
#include <thread>

#include "LocalHttpServer.hpp"
#include "TestSupport.hpp"
#include "akeno/mods/ModCheck.hpp"
#include "akeno/security/Sha256.hpp"
#include "akeno/ui/AppController.hpp"

using namespace akeno;
using namespace akeno::ui;

namespace {

// Runs fetches synchronously and remembers the results.
class FakeImageLoader final : public IImageLoader {
public:
    std::map<std::string, Result<std::string>> results;

    void ensure(const std::string& key, int, int, std::function<Result<std::string>()> fetch) override {
        if (results.count(key) == 0) results.emplace(key, fetch());
    }
    void retryFailed() override {}
};

std::string pngBytes() {
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    auto be32 = [&](std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((v >> shift) & 0xFF));
    };
    be32(13);
    bytes += "IHDR";
    be32(64);
    be32(36);
    bytes += std::string("\x08\x02\x00\x00\x00", 5);
    bytes += std::string(32, '\0');
    return bytes;
}

test::CannedResponse serve(const test::ReceivedRequest& request) {
    test::CannedResponse response;
    const std::string prefix = "/catalog/";
    if (request.target.rfind(prefix, 0) == 0) {
        const std::string path = request.target.substr(prefix.size());
        if (path.find("..") == std::string::npos && std::filesystem::is_regular_file(test::fixturePath("catalog/" + path))) {
            response.body = test::readFixture("catalog/" + path);
            return response;
        }
    } else if (request.target.rfind("/files/", 0) == 0) {
        const std::string name = request.target.substr(7);
        if (name.find("..") == std::string::npos &&
            std::filesystem::is_regular_file(test::fixturePath("catalog/files/" + name))) {
            response.contentType = "application/zip";
            response.body = test::readFixture("catalog/files/" + name);
            return response;
        }
    } else if (request.target == "/img/ok.png") {
        response.contentType = "image/png";
        response.body = pngBytes();
        return response;
    } else if (request.target == "/img/not-an-image.png") {
        response.contentType = "image/png";
        response.body = "<html>surprise</html>";
        return response;
    }
    response.status = 404;
    response.body = "{}";
    return response;
}

template <typename Condition>
bool pumpUntil(MainThreadQueue& queue, Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        queue.drain();
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

struct ControllerFixture {
    test::TempDir dir;
    test::LocalHttpServer server{serve};
    std::string base = "http://127.0.0.1:" + std::to_string(server.port());
    std::unique_ptr<app::AppContext> context;
    TaskRunner tasks{2};
    MainThreadQueue queue;
    FakeImageLoader images;
    std::unique_ptr<AppController> controller;

    ControllerFixture() {
        app::CommandLine commandLine;
        commandLine.dataRoot = dir.path() / "data";
        commandLine.shadowMountPort = 9;  // nothing listens there; not used by these tests
        commandLine.catalogueUrl = base + "/catalog/";
        auto created = app::AppContext::create(commandLine, std::make_unique<test::FakePlatform>());
        REQUIRE(created.ok());
        context = std::move(created).value();
        controller = std::make_unique<AppController>(*context, tasks, queue, &images);
    }
    ~ControllerFixture() {
        tasks.shutdown();
        queue.drain(100000);
        controller.reset();
        context.reset();
    }
    const AppViewState& state() const { return controller->state(); }
};

}  // namespace

TEST_CASE("controller loads the catalogue, mod lists and details") {
    ControllerFixture f;
    REQUIRE(f.server.ok());
    CHECK(f.state().catalog.configured);
    CHECK(f.state().catalog.source == f.base + "/catalog/");

    f.controller->loadCatalogGames(false);
    CHECK(f.state().catalog.loading);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().catalog.loading; }));
    REQUIRE(f.state().catalog.loaded);
    CHECK(f.state().catalog.games.size() == 3);
    CHECK(f.state().catalog.findByTitleId("PPSA90001") != nullptr);

    providers::SearchQuery query;
    query.game = providers::GameContext{"PPSA90001", "01.011.000"};
    f.controller->loadModList(query);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().modList.loading; }));
    REQUIRE(f.state().modList.page.has_value());
    CHECK(f.state().modList.page->total == 6);

    // Only the newest request is shown.
    query.text = "hair";
    f.controller->loadModList(query);
    query.text = "foliage";
    f.controller->loadModList(query);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().modList.loading; }));
    CHECK(f.state().modList.query.text == "foliage");
    REQUIRE(f.state().modList.page.has_value());
    REQUIRE(f.state().modList.page->mods.size() == 1);
    CHECK(f.state().modList.page->mods[0].name == "Sharper Foliage Textures");

    const providers::ModRef ref{"akeno-catalogue", "example-blade/crimson-outfit"};
    f.controller->loadModDetails(ref, query.game);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().modDetail.loading; }));
    REQUIRE(f.state().modDetail.details.has_value());
    CHECK(f.state().modDetail.details->summary.compatibility == providers::CompatibilityStatus::Verified);
    CHECK(f.state().modDetail.details->installable);

    f.controller->loadModDetails({"akeno-catalogue", "example-blade/does-not-exist"}, query.game);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().modDetail.loading; }));
    CHECK_FALSE(f.state().modDetail.details.has_value());
    REQUIRE(f.state().modDetail.error.has_value());
    CHECK(f.state().modDetail.error->code == ErrorCode::NotFound);
}

TEST_CASE("changing the catalogue address drops results for the old one") {
    ControllerFixture f;
    f.controller->loadCatalogGames(false);
    database::Settings settings = f.state().settings;
    settings.catalogueUrl = "https://catalog.example/akeno/";
    REQUIRE(f.controller->saveSettings(settings).ok());
    // The command-line address stays in effect for this session, but the view starts over.
    CHECK_FALSE(f.state().catalog.loading);
    CHECK_FALSE(f.state().catalog.loaded);
    REQUIRE(pumpUntil(f.queue, [&] { return f.queue.pending() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    f.queue.drain();
    CHECK_FALSE(f.state().catalog.loaded);  // the earlier result was discarded

    f.controller->loadCatalogGames(false);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().catalog.loading; }));
    CHECK(f.state().catalog.loaded);
}

TEST_CASE("remote images are validated, cached and limited to safe addresses") {
    ControllerFixture f;
    const std::string url = f.base + "/img/ok.png";
    const std::string key = f.controller->remoteImageKey(url, 192, 108);
    REQUIRE_FALSE(key.empty());
    CHECK(key.find(url) == std::string::npos);  // keys are hashes, not addresses
    REQUIRE(f.images.results.count(key) == 1);
    CHECK(f.images.results.at(key).ok());
    const auto requestsAfterFirst = f.server.received().size();

    // Another size of the same picture comes from the disk cache.
    const std::string larger = f.controller->remoteImageKey(url, 640, 360);
    CHECK(larger != key);
    REQUIRE(f.images.results.at(larger).ok());
    CHECK(f.server.received().size() == requestsAfterFirst);

    const std::string bad = f.controller->remoteImageKey(f.base + "/img/not-an-image.png", 192, 108);
    REQUIRE(f.images.results.count(bad) == 1);
    CHECK_FALSE(f.images.results.at(bad).ok());
    std::size_t cached = 0;
    for (const auto& entry : std::filesystem::directory_iterator(f.context->paths().imageCache())) {
        (void)entry;
        ++cached;
    }
    CHECK(cached == 1);  // the invalid response was not stored

    CHECK(f.controller->remoteImageKey("http://example.com/a.png", 100, 100).empty());
    CHECK(f.controller->remoteImageKey("ftp://example.com/a.png", 100, 100).empty());
    CHECK(f.controller->remoteImageKey("", 100, 100).empty());
    CHECK(f.controller->remoteImageKey(url, 0, 100).empty());
    CHECK(f.controller->remoteImageKey(url, 5000, 100).empty());
}

TEST_CASE("text entry edits UTF-8 text and reports the result once") {
    ControllerFixture f;
    int calls = 0;
    std::optional<std::string> result;
    f.controller->requestTextInput("Search", "ab", [&](std::optional<std::string> text) {
        ++calls;
        result = std::move(text);
    });
    CHECK(f.state().textEntry.active);
    CHECK_FALSE(f.state().textEntry.systemKeyboard);  // desktop test platform
    f.controller->appendTextInput("c\xC3\xA9");
    CHECK(f.state().textEntry.text == "abc\xC3\xA9");
    f.controller->eraseTextInput();
    CHECK(f.state().textEntry.text == "abc");
    f.controller->appendTextInput("\x01 ");
    f.controller->finishTextInput(true);
    CHECK_FALSE(f.state().textEntry.active);
    CHECK(calls == 1);
    REQUIRE(result.has_value());
    CHECK(*result == std::string("abc\xEF\xBF\xBD"));  // control characters never reach the caller
    f.controller->finishTextInput(true);
    CHECK(calls == 1);

    f.controller->requestTextInput("Search", "", [&](std::optional<std::string> text) {
        ++calls;
        result = std::move(text);
    });
    f.controller->appendTextInput(std::string(1000, 'x'));
    CHECK(f.state().textEntry.text.size() == AppController::kMaxTextInputBytes);
    f.controller->finishTextInput(false);
    CHECK(calls == 2);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("download requests are built from the catalogue and checked against the rules") {
    ControllerFixture f;
    const providers::GameContext game{"PPSA90001", "01.011.000"};
    auto pumpNotices = [&](std::size_t count) {
        REQUIRE(pumpUntil(f.queue, [&] { return f.state().notices.size() >= count; }));
        return f.state().notices.back();
    };

    f.controller->startDownload({"akeno-catalogue", "example-blade/photo-mode-ue4ss"}, game, true);
    auto notice = pumpNotices(1);
    CHECK(notice.kind == ToastKind::Error);
    CHECK(notice.text.find("Akeno will not download this mod") != std::string::npos);

    f.controller->startDownload({"akeno-catalogue", "example-blade/sharper-foliage"}, game, false);
    notice = pumpNotices(2);
    CHECK(notice.kind == ToastKind::Error);
    CHECK(notice.text.find("EXPERIMENTAL") != std::string::npos);
    CHECK(f.context->downloads().snapshot().empty());

    f.controller->startDownload({"akeno-catalogue", "example-blade/crimson-outfit"}, game, false);
    notice = pumpNotices(3);
    CHECK(notice.kind == ToastKind::Success);
    CHECK(notice.text == "Added to Downloads: Crimson Outfit Recolour");
    auto items = f.context->downloads().snapshot();
    REQUIRE(items.size() == 1);
    const auto& request = items[0].record.request;
    CHECK(request.modVersion == "1.2.0");
    CHECK(request.compatibility == "VERIFIED");
    CHECK(request.gameVersion == "01.011.000");
    CHECK(request.format == mods::ArchiveFormat::Zip);
    CHECK(request.expectedSha256.size() == 64);
    CHECK(f.state().downloads.items.size() == 1);

    f.controller->startDownload({"akeno-catalogue", "example-blade/crimson-outfit"}, game, false);
    notice = pumpNotices(4);
    CHECK(notice.text == "Already in Downloads: Crimson Outfit Recolour");

    // Experimental with confirmation is accepted.
    f.controller->startDownload({"akeno-catalogue", "example-blade/sharper-foliage"}, game, true);
    notice = pumpNotices(5);
    CHECK(notice.kind == ToastKind::Success);
    CHECK(f.context->downloads().snapshot().size() == 2);

    REQUIRE(f.controller->removeDownload(items[0].record.id).ok());
    CHECK(f.state().downloads.items.size() == 1);
}

TEST_CASE("a completed download is checked in staging and the result is kept") {
    ControllerFixture f;
    const std::string name = "example-blade-crimson-outfit-1.2.0.zip";
    const std::string archive = test::readFixture("catalog/files/" + name);
    downloads::DownloadRequest request;
    request.mod = {"akeno-catalogue", "example-blade/crimson-outfit"};
    request.displayName = "Crimson Outfit Recolour";
    request.modVersion = "1.2.0";
    request.gameTitleId = "PPSA90001";
    request.compatibility = "VERIFIED";
    request.catalogueInstallable = true;
    request.url = f.base + "/files/" + name;
    request.expectedSize = archive.size();
    request.expectedSha256 = security::sha256Hex(archive);
    request.format = mods::ArchiveFormat::Zip;
    auto manifest = test::readFixture("catalog/mods/example-blade/crimson-outfit.json");
    auto parsed = mods::parseModManifest(manifest, "crimson-outfit");
    REQUIRE(parsed.ok());
    request.archiveRoot = parsed->archiveRoot;
    request.targetPrefix = parsed->targetPrefix;

    auto& manager = f.context->downloads();
    REQUIRE(manager.start().ok());
    auto queued = manager.enqueue(request);
    REQUIRE(queued.ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (manager.find(queued->id)->record.state != downloads::DownloadState::Completed &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(manager.find(queued->id)->record.state == downloads::DownloadState::Completed);

    f.controller->start();  // refreshes the download list (and runs the system check)
    REQUIRE(pumpUntil(f.queue, [&] { return f.state().downloads.find(queued->id) != nullptr; }));
    f.controller->checkDownload(queued->id, false);
    CHECK(f.state().check.running);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().check.running; }));
    REQUIRE(f.state().check.report.has_value());
    const auto& report = *f.state().check.report;
    CHECK(report.analysis.installable);
    CHECK(report.analysis.installCount > 0);
    REQUIRE(report.plan.has_value());
    CHECK_FALSE(report.plan->changesGameFiles);
    CHECK(std::filesystem::exists(mods::reportPath(f.context->paths(), queued->id)));
    CHECK(std::filesystem::is_empty(f.context->paths().staging()));
    CHECK_FALSE(std::filesystem::exists(f.context->paths().operationJournal()));

    // The stored result is shown the next time without unpacking again.
    f.controller->checkDownload(queued->id, false);
    REQUIRE(pumpUntil(f.queue, [&] { return !f.state().check.running; }));
    CHECK(f.state().check.report.has_value());

    // Removing the download removes its result too.
    REQUIRE(f.controller->removeDownload(queued->id).ok());
    CHECK_FALSE(std::filesystem::exists(mods::reportPath(f.context->paths(), queued->id)));
}
