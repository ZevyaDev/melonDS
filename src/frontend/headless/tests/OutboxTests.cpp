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

#include "TestHarness.h"
#include "Outbox.h"
#include "Protocol.h"

using namespace Headless;
using namespace Headless::Protocol;

static std::vector<uint8_t> Frame(uint64_t frame, size_t pad = 0)
{
    FrameEvent ev;
    ev.Frame = frame;
    SubBlock b;
    b.SubId = 1;
    b.Ranges.push_back(std::vector<uint8_t>(pad, 0xAA));
    ev.Blocks.push_back(b);
    return Pack(0, ev);
}

static uint32_t DroppedOf(const std::vector<uint8_t>& msg)
{
    Message m;
    FrameParser p;
    p.Feed(msg.data(), msg.size());
    CHECK(p.Next(m));
    FrameEvent ev;
    CHECK(Decode(m.Body, ev));
    return ev.Dropped;
}

static std::vector<uint8_t> Reply(uint32_t seq)
{
    return Pack(0, OkReply{seq});
}

TEST(dropped_offset_matches_the_encoding)
{
    FrameEvent ev;
    ev.Dropped = 0x04030201;
    auto msg = Pack(0, ev);
    CHECK(msg.size() >= FrameDroppedOffset + 4);
    CHECK_EQ(msg[FrameDroppedOffset], 1);
    CHECK_EQ(msg[FrameDroppedOffset + 3], 4);
}

TEST(outbox_is_fifo_and_counts_bytes)
{
    Outbox box(1 << 20);
    auto a = Reply(1), b = Frame(2), c = Reply(3);
    box.PushReliable(a);
    CHECK(box.PushFrame(b));
    box.PushReliable(c);
    CHECK_EQ(box.Bytes(), a.size() + b.size() + c.size());
    std::vector<uint8_t> out;
    CHECK(box.Take(out)); CHECK(out == a);
    CHECK(box.Take(out)); CHECK(out.size() == b.size());
    CHECK_EQ(box.Bytes(), a.size() + b.size() + c.size());     // taken bytes stay counted until released
    box.Release(a.size());
    CHECK_EQ(box.Bytes(), b.size() + c.size());
    CHECK(box.Take(out)); CHECK(out == c);
    box.Release(b.size());
    box.Release(c.size());
    CHECK_EQ(box.Bytes(), size_t(0));
    CHECK(box.Empty());
    CHECK(!box.Take(out));
}

TEST(outbox_drops_oldest_frame_events_and_reports_the_count)
{
    size_t one = Frame(0, 1000).size();
    Outbox box(one * 3 + 10);
    for (uint64_t i = 1; i <= 5; i++)
        CHECK(box.PushFrame(Frame(i, 1000)));
    CHECK_EQ(box.TotalDropped(), uint64_t(2));
    std::vector<uint8_t> out;
    CHECK(box.Take(out));
    Message m;
    FrameParser p;
    p.Feed(out.data(), out.size());
    CHECK(p.Next(m));
    FrameEvent ev;
    CHECK(Decode(m.Body, ev));
    CHECK_EQ(ev.Frame, uint64_t(3));        // frames 1 and 2 were dropped
    CHECK_EQ(ev.Dropped, 0u);               // the count rode on the event that forced the drop
    CHECK(box.Take(out));
    CHECK_EQ(DroppedOf(out), 1u);
    CHECK(box.Take(out));
    CHECK_EQ(DroppedOf(out), 1u);          // frame 5 forced the drop of frame 2; frame 4's drop rode on frame 4
}

TEST(outbox_never_drops_replies)
{
    size_t one = Frame(0, 1000).size();
    Outbox box(one * 2);
    box.PushReliable(Reply(1));
    CHECK(box.PushFrame(Frame(1, 1000)));
    box.PushReliable(Reply(2));
    CHECK(box.PushFrame(Frame(2, 1000)));
    box.PushReliable(Reply(3));
    CHECK(box.PushFrame(Frame(3, 1000)));
    int replies = 0, frames = 0;
    std::vector<uint8_t> out;
    while (box.Take(out))
    {
        FrameParser p;
        p.Feed(out.data(), out.size());
        Message m;
        CHECK(p.Next(m));
        (m.Head.Type == uint16_t(MsgType::ok) ? replies : frames)++;
    }
    CHECK_EQ(replies, 3);
    CHECK(frames >= 1);
}

TEST(outbox_taken_message_is_not_dropped)
{
    size_t one = Frame(0, 1000).size();
    Outbox box(one + 10);
    CHECK(box.PushFrame(Frame(1, 1000)));
    std::vector<uint8_t> out;
    CHECK(box.Take(out));                   // in flight: still counted, cannot be dropped
    CHECK(!box.PushFrame(Frame(2, 1000)));  // does not fit next to it, nothing left to drop
    box.Release(out.size());
    CHECK(box.PushFrame(Frame(3, 1000)));
    CHECK(box.Take(out));
    CHECK_EQ(DroppedOf(out), 1u);
}

TEST(outbox_clear_forgets_everything_queued)
{
    Outbox box(1 << 20);
    box.PushReliable(Reply(1));
    box.PushFrame(Frame(1));
    box.Clear();
    CHECK(box.Empty());
    CHECK_EQ(box.Bytes(), size_t(0));
}

TEST(outbox_forwards_the_count_of_a_dropped_event)
{
    size_t one = Frame(0, 1000).size();
    Outbox box(one * 2 + 10);
    for (uint64_t i = 1; i <= 6; i++)
        CHECK(box.PushFrame(Frame(i, 1000)));
    // frames 1-4 are gone; the two left account for all four
    std::vector<uint8_t> out;
    uint32_t sum = 0;
    int left = 0;
    while (box.Take(out))
    {
        sum += DroppedOf(out);
        left++;
    }
    CHECK_EQ(left, 2);
    CHECK_EQ(sum, 4u);
}
