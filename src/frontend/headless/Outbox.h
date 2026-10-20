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

#ifndef HEADLESS_OUTBOX_H
#define HEADLESS_OUTBOX_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace Headless
{

// Byte offset of FrameEvent::Dropped inside a complete frame message (header included).
constexpr size_t FrameDroppedOffset = 8 + 4 + 8 + 4 + 1 + 1 + 2;

// Outgoing message queue of one connection. Any thread may push; the server thread takes
// messages in order and writes them. The queue is bounded: when a frame event would not
// fit, the oldest queued frame events are dropped (replies and other events never are) and
// the count goes into the Dropped field of the next frame event that is queued.
class Outbox
{
public:
    explicit Outbox(size_t capacity = 64u * 1024 * 1024) : Capacity(capacity) {}

    // Replies, break events and the like. Never dropped, may exceed the capacity.
    void PushReliable(std::vector<uint8_t> msg);

    // A complete frame event message. Its Dropped field is set here. Returns false if the
    // message itself had to be dropped (it alone does not fit).
    bool PushFrame(std::vector<uint8_t> msg);

    // Move the oldest message out for sending. The bytes stay counted until Release().
    bool Take(std::vector<uint8_t>& out);
    void Release(size_t bytes);

    bool Empty() const;
    size_t Bytes() const;           // queued plus taken but not yet released
    uint64_t TotalDropped() const;
    void Clear();                   // queued messages and the pending drop count (taken ones stay counted)

private:
    struct Item
    {
        std::vector<uint8_t> Data;
        bool Droppable;
    };

    size_t Capacity;
    mutable std::mutex Lock;
    std::deque<Item> Items;
    size_t Queued = 0;
    size_t InFlight = 0;
    uint32_t Pending = 0;
    uint64_t Total = 0;
};

}

#endif
