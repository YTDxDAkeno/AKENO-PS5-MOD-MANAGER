// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <vector>

#include "akeno/app/CommandLine.hpp"

using namespace akeno::app;

namespace {

CommandLine parse(std::vector<const char*> args) {
    args.insert(args.begin(), "AkenoModManager");
    return parseCommandLine(static_cast<int>(args.size()), args.data());
}

}  // namespace

TEST_CASE("no arguments starts the user interface") {
    auto cl = parse({});
    CHECK(cl.mode == RunMode::Interactive);
    CHECK(cl.errors.empty());
}

TEST_CASE("headless modes and options") {
    auto cl = parse({"--self-check", "--data-root", "/data/akeno-test", "--shadowmount-port", "10102", "--verbose"});
    CHECK(cl.errors.empty());
    CHECK(cl.mode == RunMode::SelfCheck);
    CHECK(cl.dataRoot == std::filesystem::path("/data/akeno-test"));
    CHECK(cl.shadowMountPort == std::optional<int>(10102));
    CHECK(cl.verbose);
    CHECK(parse({"--list-games"}).mode == RunMode::ListGames);
    auto catalogue = parse({"--catalogue-url", "http://127.0.0.1:10101/catalog/"});
    CHECK(catalogue.errors.empty());
    CHECK(catalogue.catalogueUrl == std::optional<std::string>("http://127.0.0.1:10101/catalog/"));
    CHECK_FALSE(parse({"--catalogue-url"}).errors.empty());
    CHECK(parse({"--download-test"}).mode == RunMode::DownloadTest);
    auto custom = parse({"--download-test", "--download-test-url", "http://127.0.0.1:10101/files/a.zip",
                         "--download-test-sha256", "abc", "--download-test-size", "244"});
    CHECK(custom.errors.empty());
    CHECK(custom.downloadTestSize == std::optional<std::uint64_t>(244));
    CHECK_FALSE(parse({"--download-test", "--download-test-url", "https://x/a.zip"}).errors.empty());
    CHECK_FALSE(parse({"--download-test-size", "0"}).errors.empty());
    auto window = parse({"--window", "1920x1080"});
    CHECK(window.windowWidth == 1920);
    CHECK(window.windowHeight == 1080);
}

TEST_CASE("invalid arguments are reported") {
    CHECK_FALSE(parse({"--data-root", "relative"}).errors.empty());
    CHECK_FALSE(parse({"--data-root"}).errors.empty());
    CHECK_FALSE(parse({"--shadowmount-port", "0"}).errors.empty());
    CHECK_FALSE(parse({"--shadowmount-port", "abc"}).errors.empty());
    CHECK_FALSE(parse({"--window", "big"}).errors.empty());
    CHECK_FALSE(parse({"--unknown"}).errors.empty());
}
