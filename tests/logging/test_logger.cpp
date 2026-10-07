// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <fstream>
#include <sstream>

#include "TestSupport.hpp"
#include "akeno/logging/Logger.hpp"

using namespace akeno;
using namespace akeno::logging;

TEST_CASE("logger redacts before any sink sees the message") {
    Logger log;
    auto ring = std::make_shared<RingBufferSink>(10);
    log.addSink(ring);
    log.info("network", "request ?api_key=SECRET&x=1");
    auto records = ring->snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records[0].message.find("SECRET") == std::string::npos);
    CHECK(records[0].message.find("[REDACTED]") != std::string::npos);
    CHECK(records[0].category == "network");
}

TEST_CASE("minimum level filters debug messages") {
    Logger log;
    auto ring = std::make_shared<RingBufferSink>(10);
    log.addSink(ring);
    log.debug("x", "hidden");
    log.setMinimumLevel(LogLevel::Debug);
    log.debug("x", "shown");
    auto records = ring->snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records[0].message == "shown");
}

TEST_CASE("ring buffer keeps only the newest records") {
    RingBufferSink ring(3);
    for (int i = 0; i < 5; ++i) {
        LogRecord record;
        record.message = std::to_string(i);
        ring.write(record);
    }
    auto records = ring.snapshot();
    REQUIRE(records.size() == 3);
    CHECK(records.front().message == "2");
    CHECK(records.back().message == "4");
}

TEST_CASE("rotating file sink rotates by size and keeps a bounded number of files") {
    test::TempDir dir;
    RotatingFileSink sink(dir.path(), 200, 3);
    for (int i = 0; i < 40; ++i) {
        LogRecord record;
        record.timestamp = "2026-10-07T00:00:00Z";
        record.category = "test";
        record.message = "line " + std::to_string(i);
        sink.write(record);
    }
    sink.flush();
    CHECK(std::filesystem::exists(dir.path() / "akeno.log"));
    CHECK(std::filesystem::exists(dir.path() / "akeno.log.1"));
    CHECK(std::filesystem::exists(dir.path() / "akeno.log.2"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "akeno.log.3"));
    CHECK(std::filesystem::file_size(dir.path() / "akeno.log") <= 200);
    std::ifstream in(dir.path() / "akeno.log");
    std::stringstream content;
    content << in.rdbuf();
    CHECK(content.str().find("line 39") != std::string::npos);
}

TEST_CASE("log lines are single-line and bounded") {
    Logger log;
    auto ring = std::make_shared<RingBufferSink>(10);
    log.addSink(ring);
    log.error("x", "multi\nline");
    log.error("x", std::string(10000, 'a'));
    auto records = ring->snapshot();
    REQUIRE(records.size() == 2);
    CHECK(records[0].message == "multi line");
    CHECK(records[1].message.size() <= 2048);
}
