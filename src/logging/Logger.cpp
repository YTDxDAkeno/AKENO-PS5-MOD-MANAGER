// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/logging/Logger.hpp"

#include <cstdio>
#include <system_error>

#include "akeno/core/Limits.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/logging/Redactor.hpp"

namespace akeno::logging {

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

std::string LogRecord::format() const {
    std::string line;
    line.reserve(timestamp.size() + category.size() + message.size() + 16);
    line.append(timestamp).append(" ");
    std::string_view levelName = toString(level);
    line.append(levelName);
    line.append(levelName.size() < 5 ? 5 - levelName.size() : 0, ' ');
    line.append(" [").append(category).append("] ").append(message);
    return line;
}

// ---------------------------------------------------------------- RotatingFileSink

RotatingFileSink::RotatingFileSink(std::filesystem::path directory, std::size_t maxBytes, int maxFiles)
    : directory_(std::move(directory)), maxBytes_(maxBytes), maxFiles_(maxFiles < 1 ? 1 : maxFiles) {}

void RotatingFileSink::openIfNeeded() {
    if (stream_.is_open() || failed_) {
        return;
    }
    std::error_code ec;
    auto path = currentFile();
    currentBytes_ = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (ec) {
        currentBytes_ = 0;
    }
    stream_.open(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream_.is_open()) {
        failed_ = true;  // logging must never take the application down
    }
}

void RotatingFileSink::rotate() {
    stream_.close();
    std::error_code ec;
    for (int index = maxFiles_ - 1; index >= 1; --index) {
        auto from = index == 1 ? currentFile() : directory_ / ("akeno.log." + std::to_string(index - 1));
        auto to = directory_ / ("akeno.log." + std::to_string(index));
        if (std::filesystem::exists(from, ec)) {
            std::filesystem::rename(from, to, ec);
        }
    }
    if (maxFiles_ == 1) {
        std::filesystem::remove(currentFile(), ec);
    }
    currentBytes_ = 0;
    failed_ = false;
    openIfNeeded();
}

void RotatingFileSink::write(const LogRecord& record) {
    openIfNeeded();
    if (!stream_.is_open()) {
        return;
    }
    std::string line = record.format();
    line.push_back('\n');
    if (currentBytes_ + line.size() > maxBytes_ && currentBytes_ > 0) {
        rotate();
        if (!stream_.is_open()) {
            return;
        }
    }
    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    currentBytes_ += line.size();
    if (record.level >= LogLevel::Warn) {
        stream_.flush();  // keep errors on disk even if the process dies afterwards
    }
}

void RotatingFileSink::flush() {
    if (stream_.is_open()) {
        stream_.flush();
    }
}

// ---------------------------------------------------------------- RingBufferSink

RingBufferSink::RingBufferSink(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void RingBufferSink::write(const LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.size() >= capacity_) {
        records_.pop_front();
    }
    records_.push_back(record);
}

std::vector<LogRecord> RingBufferSink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {records_.begin(), records_.end()};
}

// ---------------------------------------------------------------- StderrSink

void StderrSink::write(const LogRecord& record) {
    std::string line = record.format();
    std::fprintf(stderr, "%s\n", line.c_str());
}

// ---------------------------------------------------------------- Logger

void Logger::addSink(std::shared_ptr<ILogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::clearSinks() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        sink->flush();
    }
    sinks_.clear();
}

void Logger::setMinimumLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    minimum_ = level;
}

LogLevel Logger::minimumLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return minimum_;
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level < minimum_ || sinks_.empty()) {
        return;
    }
    LogRecord record;
    record.level = level;
    record.timestamp = strings::utcTimestamp();
    record.category = strings::sanitizeForDisplay(category, 32);
    record.message = strings::sanitizeForDisplay(redactSecrets(message), limits::kMaxLogLineBytes);
    for (auto& sink : sinks_) {
        sink->write(record);
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        sink->flush();
    }
}

Logger& logger() {
    static Logger instance;
    return instance;
}

}  // namespace akeno::logging
