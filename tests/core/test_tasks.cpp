// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "akeno/core/Tasks.hpp"

using namespace akeno;

TEST_CASE("TaskRunner runs every submitted job before shutdown returns") {
    std::atomic<int> counter{0};
    {
        TaskRunner runner(3);
        for (int i = 0; i < 100; ++i) {
            runner.submit([&] { counter.fetch_add(1); });
        }
        runner.shutdown();
    }
    CHECK(counter.load() == 100);
}

TEST_CASE("TaskRunner survives a throwing job") {
    std::atomic<int> counter{0};
    TaskRunner runner(1);
    runner.submit([] { throw std::runtime_error("boom"); });
    runner.submit([&] { counter.fetch_add(1); });
    runner.shutdown();
    CHECK(counter.load() == 1);
}

TEST_CASE("Jobs submitted after shutdown are ignored") {
    std::atomic<int> counter{0};
    TaskRunner runner(1);
    runner.shutdown();
    runner.submit([&] { counter.fetch_add(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(counter.load() == 0);
}

TEST_CASE("MainThreadQueue runs posted work only when drained") {
    MainThreadQueue queue;
    int value = 0;
    queue.post([&] { value += 1; });
    queue.post([&] { value += 10; });
    CHECK(value == 0);
    CHECK(queue.pending() == 2);
    CHECK(queue.drain(1) == 1);
    CHECK(value == 1);
    CHECK(queue.drain() == 1);
    CHECK(value == 11);
    CHECK(queue.drain() == 0);
}

TEST_CASE("CancellationToken copies share state") {
    CancellationToken token;
    CancellationToken copy = token;
    CHECK_FALSE(copy.cancelled());
    token.cancel();
    CHECK(copy.cancelled());
}
