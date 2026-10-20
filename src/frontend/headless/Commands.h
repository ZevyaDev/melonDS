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

#ifndef HEADLESS_COMMANDS_H
#define HEADLESS_COMMANDS_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace Headless
{

// Work posted by other threads for one emulator thread to run. The emulator
// thread calls Drain() at frame boundaries, while parked at a break, and when
// woken from idle, so nothing else touches the NDS mid-frame.
class CommandQueue
{
public:
    using Fn = std::function<void()>;

    ~CommandQueue();

    // Called after every Post/Call, on the posting thread, outside the queue's lock.
    // Set before the queue is shared between threads.
    void SetWake(std::function<void()> wake);

    // The thread that calls Drain(). A Call() made on that thread runs inline.
    void SetDrainThread(std::thread::id id);
    bool OnDrainThread() const;

    // Queue fn and return. False if the queue is shut down (fn is dropped).
    bool Post(Fn fn);

    // Run fn on the drain thread and wait for it to finish. Runs inline when
    // called from the drain thread. False if the queue shut down before fn ran.
    bool Call(Fn fn);

    // Run everything queued so far, in FIFO order. Drain thread only.
    size_t Drain();

    bool HasPending() const { return Count.load() != 0; }

    // Fail every pending and future Call/Post. Never leaves a caller blocked.
    void Shutdown();

private:
    struct Reply
    {
        std::mutex Lock;
        std::condition_variable Cond;
        bool Done = false;
        bool Ran = false;
    };

    struct Item
    {
        Fn Func;
        std::shared_ptr<Reply> Result;
    };

    bool Push(Item item);
    static void Finish(Item& item, bool ran);

    mutable std::mutex Lock;
    std::deque<Item> Items;
    std::atomic<size_t> Count {0};
    std::thread::id Drainer;
    bool Closed = false;
    std::function<void()> Wake;
};

}

#endif
