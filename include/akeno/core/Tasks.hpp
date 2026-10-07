// SPDX-License-Identifier: GPL-3.0-or-later
// Background workers and the queue that hands their results back to the UI thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace akeno {

// Cooperative cancellation flag shared between the requester and the worker.
class CancellationToken {
public:
    void cancel() noexcept { cancelled_->store(true); }
    bool cancelled() const noexcept { return cancelled_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

// Closures posted here run on the UI thread when it calls drain().
class MainThreadQueue {
public:
    void post(std::function<void()> work);
    // Runs at most `maxItems` queued closures. Returns how many ran.
    std::size_t drain(std::size_t maxItems = 64);
    std::size_t pending() const;

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

// Fixed pool of worker threads. Exceptions thrown by a job are caught and logged so a single
// bad job cannot terminate the process.
class TaskRunner {
public:
    explicit TaskRunner(std::size_t workerCount);
    ~TaskRunner();

    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;

    void submit(std::function<void()> job);
    // Stops accepting work, finishes queued jobs, joins all workers. Idempotent.
    void shutdown();

private:
    void workerLoop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

}  // namespace akeno
