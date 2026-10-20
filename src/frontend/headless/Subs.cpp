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

#include "Subs.h"

#include <algorithm>

#include "ApiInternal.h"
#include "Instance.h"
#include "Protocol.h"

namespace Headless
{

static std::mutex SinkLock;
static Subscriptions::Sink TheSink;

void Subscriptions::SetSink(Sink sink)
{
    std::lock_guard lock(SinkLock);     // waits for a Publish that is delivering an event
    TheSink = std::move(sink);
}

// A frame event must fit in one message with room to spare.
static constexpr uint64_t MaxEventBytes = Protocol::MaxBody - 4096;

bool Subscriptions::Set(uint32_t id, uint32_t everyN, std::vector<SubRangeSpec> ranges)
{
    std::lock_guard lock(Lock);
    uint64_t total = 0;
    for (const Sub& s : List)
        if (s.Id != id)
            for (const SubRangeSpec& r : s.Ranges)
                total += r.Len + 4;
    for (const SubRangeSpec& r : ranges)
        total += r.Len + 4;
    if (total > MaxEventBytes)
        return false;
    Sub sub {id, everyN ? everyN : 1, std::move(ranges)};
    auto it = std::find_if(List.begin(), List.end(), [id](const Sub& s) { return s.Id == id; });
    if (it != List.end())
        *it = std::move(sub);
    else
        List.push_back(std::move(sub));
    Count = List.size();
    return true;
}

bool Subscriptions::Remove(uint32_t id)
{
    std::lock_guard lock(Lock);
    auto it = std::find_if(List.begin(), List.end(), [id](const Sub& s) { return s.Id == id; });
    if (it == List.end())
        return false;
    List.erase(it);
    Count = List.size();
    return true;
}

void Subscriptions::Clear()
{
    std::lock_guard lock(Lock);
    List.clear();
    Count = 0;
}

void Subscriptions::Publish(Instance& inst, const AppliedInput& applied)
{
    if (!Count.load())
        return;

    Protocol::FrameEvent ev;
    ev.Frame = inst.FramesRun;
    ev.AppliedSeq = applied.Seq;
    ev.InputApplied = applied.Consumed;
    // Wait mode with nothing queued: the console will stop at the next frame boundary for input.
    ev.WaitingForInput = inst.Input.Mode() == EmptyMode::Wait && inst.Input.Depth() == 0;
    ev.QueueDepth = uint16_t(std::min<size_t>(inst.Input.Depth(), 0xFFFF));
    {
        std::lock_guard lock(Lock);
        for (const Sub& s : List)
        {
            if (ev.Frame % s.Every != 0)
                continue;
            Protocol::SubBlock block;
            block.SubId = s.Id;
            for (const SubRangeSpec& r : s.Ranges)
            {
                block.Ranges.emplace_back(r.Len);
                Api::ReadGuest(inst, r.Cpu, r.Addr, r.Len, block.Ranges.back().data());
            }
            ev.Blocks.push_back(std::move(block));
        }
    }
    if (ev.Blocks.empty())
        return;

    std::vector<uint8_t> msg = Protocol::Pack(uint16_t(inst.ID), ev);
    if (msg.empty())
        return;
    std::lock_guard lock(SinkLock);
    if (TheSink)
        TheSink(inst.ID, std::move(msg));
}

}
