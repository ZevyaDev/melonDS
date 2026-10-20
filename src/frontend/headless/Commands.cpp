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

#include "Commands.h"

namespace Headless
{

CommandQueue::~CommandQueue()
{
    Shutdown();
}

void CommandQueue::SetWake(std::function<void()> wake)
{
    std::lock_guard lock(Lock);
    Wake = std::move(wake);
}

void CommandQueue::SetDrainThread(std::thread::id id)
{
    std::lock_guard lock(Lock);
    Drainer = id;
}

bool CommandQueue::OnDrainThread() const
{
    std::lock_guard lock(Lock);
    return Drainer == std::this_thread::get_id();
}

bool CommandQueue::Push(Item item)
{
    std::function<void()> wake;
    {
        std::lock_guard lock(Lock);
        if (Closed)
            return false;
        Items.push_back(std::move(item));
        Count++;
        wake = Wake;
    }
    if (wake)
        wake();
    return true;
}

bool CommandQueue::Post(Fn fn)
{
    return Push({std::move(fn), nullptr});
}

bool CommandQueue::Call(Fn fn)
{
    bool inlineRun;
    {
        std::lock_guard lock(Lock);
        if (Closed)
            return false;
        inlineRun = Drainer == std::this_thread::get_id();
    }
    if (inlineRun)
    {
        // the caller is the thread that would run it anyway
        fn();
        return true;
    }

    auto reply = std::make_shared<Reply>();
    if (!Push({std::move(fn), reply}))
        return false;
    std::unique_lock lock(reply->Lock);
    reply->Cond.wait(lock, [&] { return reply->Done; });
    return reply->Ran;
}

void CommandQueue::Finish(Item& item, bool ran)
{
    if (!item.Result)
        return;
    {
        std::lock_guard lock(item.Result->Lock);
        item.Result->Done = true;
        item.Result->Ran = ran;
    }
    item.Result->Cond.notify_all();
}

size_t CommandQueue::Drain()
{
    std::deque<Item> batch;
    {
        std::lock_guard lock(Lock);
        batch.swap(Items);
        Count = 0;
    }
    for (auto& item : batch)
    {
        try
        {
            item.Func();
        }
        catch (...)
        {
            Finish(item, false);
            continue;
        }
        Finish(item, true);
    }
    return batch.size();
}

void CommandQueue::Shutdown()
{
    std::deque<Item> dropped;
    {
        std::lock_guard lock(Lock);
        Closed = true;
        dropped.swap(Items);
        Count = 0;
    }
    for (auto& item : dropped)
        Finish(item, false);
}

}
