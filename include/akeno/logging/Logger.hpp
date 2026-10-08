// SPDX-License-Identifier: GPL-3.0-or-later
// Thread-safe logger. Every message passes through redactSecrets() before any sink sees it.
#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace akeno::logging {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

std::string_view toString(LogLevel level) noexcept;

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string timestamp;  // UTC ISO-8601
    std::string category;   // "startup", "shadowmount", "network", ...
    std::string message;    // already redacted

    std::string format() const;  // "2026-10-07T11:22:33Z INFO  [startup] message"
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

// Appends to <dir>/akeno.log and rotates to akeno.log.1 .. akeno.log.(N-1) by size.
// Every record is handed to the kernel with write(2) at once (no user-space buffer), so a
// process that is killed or a console that shuts down loses nothing that was logged before.
// Warnings, errors and flush() also fsync, which survives a power loss on most filesystems.
class RotatingFileSink final : public ILogSink {
public:
    RotatingFileSink(std::filesystem::path directory, std::size_t maxBytes, int maxFiles);
    ~RotatingFileSink() override;
    RotatingFileSink(const RotatingFileSink&) = delete;
    RotatingFileSink& operator=(const RotatingFileSink&) = delete;
    void write(const LogRecord& record) override;
    void flush() override;
    std::filesystem::path currentFile() const { return directory_ / "akeno.log"; }

private:
    void openIfNeeded();
    void rotate();
    void closeFile();

    std::filesystem::path directory_;
    std::size_t maxBytes_;
    int maxFiles_;
    int fd_ = -1;
    std::size_t currentBytes_ = 0;
    bool failed_ = false;
};

// Keeps the most recent records in memory for the on-console log viewer.
class RingBufferSink final : public ILogSink {
public:
    explicit RingBufferSink(std::size_t capacity);
    void write(const LogRecord& record) override;
    std::vector<LogRecord> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<LogRecord> records_;
};

class StderrSink final : public ILogSink {
public:
    void write(const LogRecord& record) override;
};

class Logger {
public:
    void addSink(std::shared_ptr<ILogSink> sink);
    void clearSinks();
    void setMinimumLevel(LogLevel level);
    LogLevel minimumLevel() const;

    void log(LogLevel level, std::string_view category, std::string_view message);
    void debug(std::string_view category, std::string_view message) { log(LogLevel::Debug, category, message); }
    void info(std::string_view category, std::string_view message) { log(LogLevel::Info, category, message); }
    void warn(std::string_view category, std::string_view message) { log(LogLevel::Warn, category, message); }
    void error(std::string_view category, std::string_view message) { log(LogLevel::Error, category, message); }
    void flush();

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
    LogLevel minimum_ = LogLevel::Info;
};

// Process-wide logger. Sinks are attached during startup.
Logger& logger();

}  // namespace akeno::logging
