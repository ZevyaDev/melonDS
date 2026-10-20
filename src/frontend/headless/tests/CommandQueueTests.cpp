/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "TestHarness.h"
#include "Commands.h"

using Headless::CommandQueue;

// A thread that drains the queue until destroyed.
struct Drainer
{
    CommandQueue& Q;
    std::atomic<bool> Stop {false};
    std::thread T;

    explicit Drainer(CommandQueue& q) : Q(q)
    {
        T = std::thread([this] {
            while (!Stop)
            {
                Q.Drain();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            Q.Drain();
        });
        Q.SetDrainThread(T.get_id());
    }
    ~Drainer()
    {
        Stop = true;
        T.join();
    }
};

TEST(PostFromManyThreadsRunsFifoPerThread)
{
    CommandQueue q;
    constexpr int Threads = 8, Per = 500;
    std::vector<std::pair<int, int>> seen; // touched only by the draining (main) thread
    q.SetDrainThread(std::this_thread::get_id());

    std::vector<std::thread> producers;
    for (int t = 0; t < Threads; t++)
        producers.emplace_back([&, t] {
            for (int i = 0; i < Per; i++)
                q.Post([&seen, t, i] { seen.push_back({t, i}); });
        });

    while ((int)seen.size() < Threads * Per)
        q.Drain();
    for (auto& p : producers)
        p.join();

    CHECK_EQ((int)seen.size(), Threads * Per);
    std::vector<int> next(Threads, 0);
    bool ordered = true;
    for (auto& [t, i] : seen)
    {
        if (i != next[t])
            ordered = false;
        next[t] = i + 1;
    }
    CHECK(ordered);
    CHECK(!q.HasPending());
}

TEST(CallBlocksUntilRunAndSeesResult)
{
    CommandQueue q;
    Drainer d(q);
    int result = 0;
    std::thread::id ranOn;
    bool ok = q.Call([&] {
        result = 42;
        ranOn = std::this_thread::get_id();
    });
    CHECK(ok);
    CHECK_EQ(result, 42);
    CHECK(ranOn == d.T.get_id());
}

TEST(CallFromDrainThreadRunsInline)
{
    CommandQueue q;
    q.SetDrainThread(std::this_thread::get_id());
    std::vector<int> order;
    q.Post([&] {
        order.push_back(1);
        bool ok = q.Call([&] { order.push_back(2); });
        CHECK(ok);
        order.push_back(3);
    });
    q.Drain();
    CHECK(order == (std::vector<int> {1, 2, 3}));
}

TEST(WakeFiresOnPost)
{
    CommandQueue q;
    std::atomic<int> wakes {0};
    q.SetWake([&] { wakes++; });
    q.Post([] {});
    q.Post([] {});
    CHECK_EQ(wakes.load(), 2);
    CHECK(q.HasPending());
    q.Drain();
    CHECK(!q.HasPending());
}

TEST(ShutdownFailsPendingCallsWithoutHanging)
{
    CommandQueue q;
    q.SetDrainThread(std::this_thread::get_id()); // nobody drains: callers block
    constexpr int Callers = 4;
    std::atomic<int> failed {0}, started {0};
    std::atomic<bool> ranAny {false};
    std::vector<std::thread> callers;
    for (int i = 0; i < Callers; i++)
        callers.emplace_back([&] {
            started++;
            if (!q.Call([&] { ranAny = true; }))
                failed++;
        });
    while (started < Callers)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.Shutdown();
    for (auto& c : callers)
        c.join();
    CHECK_EQ(failed.load(), Callers);
    CHECK(!ranAny);
    CHECK(!q.Call([] {}));
    CHECK(!q.Post([] {}));
}
