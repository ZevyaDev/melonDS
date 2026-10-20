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

#include "Outbox.h"

namespace Headless
{

void Outbox::PushReliable(std::vector<uint8_t> msg)
{
    std::lock_guard lock(Lock);
    Queued += msg.size();
    Items.push_back({std::move(msg), false});
}

bool Outbox::PushFrame(std::vector<uint8_t> msg)
{
    std::lock_guard lock(Lock);
    // Make room by dropping the oldest frame events that nobody has started sending.
    for (auto it = Items.begin(); Queued + InFlight + msg.size() > Capacity && it != Items.end();)
    {
        if (it->Droppable)
        {
            // The event may itself be carrying earlier drops; pass those on too.
            uint32_t carried = 0;
            if (it->Data.size() >= FrameDroppedOffset + 4)
                for (int i = 0; i < 4; i++)
                    carried |= uint32_t(it->Data[FrameDroppedOffset + i]) << (8 * i);
            Queued -= it->Data.size();
            it = Items.erase(it);
            Pending += 1 + carried;
            Total++;
        }
        else
            ++it;
    }
    if (Queued + InFlight + msg.size() > Capacity)
    {
        Pending++;
        Total++;
        return false;
    }
    if (msg.size() >= FrameDroppedOffset + 4)
        for (int i = 0; i < 4; i++)
            msg[FrameDroppedOffset + i] = uint8_t(Pending >> (8 * i));
    Pending = 0;
    Queued += msg.size();
    Items.push_back({std::move(msg), true});
    return true;
}

bool Outbox::Take(std::vector<uint8_t>& out)
{
    std::lock_guard lock(Lock);
    if (Items.empty())
        return false;
    out = std::move(Items.front().Data);
    Items.pop_front();
    Queued -= out.size();
    InFlight += out.size();
    return true;
}

void Outbox::Release(size_t bytes)
{
    std::lock_guard lock(Lock);
    InFlight = bytes > InFlight ? 0 : InFlight - bytes;
}

bool Outbox::Empty() const
{
    std::lock_guard lock(Lock);
    return Items.empty();
}

size_t Outbox::Bytes() const
{
    std::lock_guard lock(Lock);
    return Queued + InFlight;
}

uint64_t Outbox::TotalDropped() const
{
    std::lock_guard lock(Lock);
    return Total;
}

void Outbox::Clear()
{
    std::lock_guard lock(Lock);
    Items.clear();
    Queued = 0;
    Pending = 0;
}

}
