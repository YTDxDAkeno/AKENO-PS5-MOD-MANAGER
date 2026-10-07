// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/core/Tasks.hpp"

#include <exception>

#include "akeno/logging/Logger.hpp"

namespace akeno {

void MainThreadQueue::post(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(work));
}

std::size_t MainThreadQueue::drain(std::size_t maxItems) {
    std::size_t ran = 0;
    while (ran < maxItems) {
        std::function<void()> work;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                break;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            work();
        } catch (const std::exception& e) {
            logging::logger().error("ui", std::string("Unhandled exception in UI callback: ") + e.what());
        } catch (...) {
            logging::logger().error("ui", "Unhandled non-standard exception in UI callback.");
        }
        ++ran;
    }
    return ran;
}

std::size_t MainThreadQueue::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

TaskRunner::TaskRunner(std::size_t workerCount) {
    if (workerCount == 0) {
        workerCount = 1;
    }
    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

TaskRunner::~TaskRunner() { shutdown(); }

void TaskRunner::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            logging::logger().warn("tasks", "Job submitted after shutdown was ignored.");
            return;
        }
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void TaskRunner::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) {
            return;
        }
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void TaskRunner::workerLoop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return;  // stopping and nothing left to do
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            logging::logger().error("tasks", std::string("Unhandled exception in background job: ") + e.what());
        } catch (...) {
            logging::logger().error("tasks", "Unhandled non-standard exception in background job.");
        }
    }
}

}  // namespace akeno
