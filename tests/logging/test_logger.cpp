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

TEST_CASE("the file sink hands every record to the kernel at once, without a flush") {
    // 0.2.0-alpha buffered INFO lines in user space; the PS5 export then read an empty akeno.log.
    test::TempDir dir;
    RotatingFileSink sink(dir.path(), 1024 * 1024, 3);
    LogRecord record;
    record.level = LogLevel::Info;
    record.timestamp = "2026-10-08T16:51:40Z";
    record.category = "install";
    record.message = "stage 3/7 files-installed: ok";
    sink.write(record);
    // Read through a separate descriptor while the sink is still open and never flushed.
    std::ifstream in(dir.path() / "akeno.log");
    std::stringstream content;
    content << in.rdbuf();
    CHECK(content.str().find("stage 3/7 files-installed: ok") != std::string::npos);
}

TEST_CASE("the file sink appends to an existing log and keeps counting its size") {
    test::TempDir dir;
    test::writeText(dir.path() / "akeno.log", std::string(150, 'x') + "\n");
    RotatingFileSink sink(dir.path(), 200, 2);
    LogRecord record;
    record.timestamp = "t";
    record.category = "c";
    record.message = std::string(80, 'y');
    sink.write(record);  // would exceed 200 bytes: rotates first
    CHECK(std::filesystem::exists(dir.path() / "akeno.log.1"));
    CHECK(std::filesystem::file_size(dir.path() / "akeno.log") < 200);
}
