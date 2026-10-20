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

#include <string>

#include "TestHarness.h"
#include "Input.h"

using namespace Headless;

static InputPayload Pay(uint32_t seq, uint16_t buttons, InputSource src = InputSource::Client)
{
    InputPayload p;
    p.Seq = seq;
    p.Buttons = buttons;
    p.Source = src;
    return p;
}

static int Put(InputQueue& q, const InputPayload& p)
{
    std::string err;
    int d = q.Submit(p, err);
    if (d < 0)
        ::TestHarness::Fail(__FILE__, __LINE__, "submit failed: " + err);
    return d;
}

TEST(InputFifoOneBufferPerFrame)
{
    InputQueue q;
    CHECK_EQ(Put(q, Pay(1, 0x1)), 1);
    CHECK_EQ(Put(q, Pay(2, 0x2)), 2);
    CHECK_EQ(Put(q, Pay(3, 0x4)), 3);
    CHECK_EQ(q.Depth(), (size_t)3);
    AppliedInput a;
    for (uint32_t i = 1; i <= 3; i++)
    {
        CHECK(q.Next(i - 1, a));
        CHECK(a.Consumed);
        CHECK_EQ(a.Seq, i);
        CHECK_EQ(a.Buttons, (uint16_t)(1u << (i - 1)));
        CHECK_EQ(q.Depth(), (size_t)(3 - i));
    }
}

TEST(InputFullAtCapacityAppliesNothing)
{
    InputQueue q;
    for (size_t i = 0; i < InputQueueCapacity; i++)
        Put(q, Pay(i + 1, 1));
    std::string err;
    CHECK_EQ(q.Submit(Pay(9999, 2), err), -1);
    CHECK(err == "input_queue_full");
    CHECK_EQ(q.Depth(), InputQueueCapacity);
    AppliedInput a;
    for (size_t i = 0; i < InputQueueCapacity; i++)
    {
        CHECK(q.Next(i, a));
        CHECK_EQ(a.Seq, (uint32_t)(i + 1));
    }
    q.Next(InputQueueCapacity, a);
    CHECK(!a.Consumed);
}

TEST(InputValidation)
{
    InputQueue q;
    std::string err;

    InputPayload p = Pay(1, 0);
    for (int i = 0; i < 17; i++)
        p.Touch.push_back({(uint32_t)i * 10, 1, 1, 1});
    CHECK_EQ(q.Submit(p, err), -1);
    CHECK(err.rfind("bad_payload", 0) == 0);

    p = Pay(1, 0);
    p.Touch = {{100, 1, 1, 1}, {100, 2, 2, 1}};     // not strictly increasing
    CHECK_EQ(q.Submit(p, err), -1);
    CHECK(err.rfind("bad_payload", 0) == 0);

    p.Touch = {{0, 1, 1, 1}, {560190, 2, 2, 1}};    // offset out of range
    CHECK_EQ(q.Submit(p, err), -1);
    CHECK(err.rfind("bad_payload", 0) == 0);

    p.Touch = {{0, 256, 1, 1}};
    CHECK_EQ(q.Submit(p, err), -1);
    p.Touch = {{0, 1, 192, 1}};
    CHECK_EQ(q.Submit(p, err), -1);

    p = Pay(1, 0);
    p.MicKind = 1;
    p.Mic.assign(797, 0);
    CHECK_EQ(q.Submit(p, err), -1);
    CHECK(err.rfind("bad_payload", 0) == 0);

    CHECK_EQ(q.Depth(), (size_t)0);

    p = Pay(1, 0);                                  // the limits themselves are fine
    for (int i = 0; i < 16; i++)
        p.Touch.push_back({(uint32_t)i * 1000, 255, 191, 1});
    p.Touch.push_back({0, 0, 0, 0});
    p.Touch.pop_back();
    p.Touch[0].Offset = 0;
    p.MicKind = 1;
    p.Mic.assign(796, 5);
    CHECK_EQ(q.Submit(p, err), 1);
    p.Touch.back().Offset = 559189;
    CHECK_EQ(q.Submit(p, err), 2);
}

TEST(InputRepeatKeepsButtonsLidTouchNotMic)
{
    InputQueue q;
    InputPayload p = Pay(1, 0x5);
    p.Lid = 1;
    p.Touch = {{0, 10, 20, 1}, {1000, 30, 40, 1}};
    p.MicKind = 1;
    p.Mic = {1, 2, 3};
    Put(q, p);

    AppliedInput a;
    CHECK(q.Next(0, a));
    CHECK(a.Consumed);
    CHECK_EQ(a.Touch.size(), (size_t)2);
    CHECK_EQ(a.Mic.size(), (size_t)3);

    CHECK(q.Next(1, a));
    CHECK(!a.Consumed);
    CHECK_EQ(a.Buttons, (uint16_t)0x5);
    CHECK_EQ(a.Lid, (uint8_t)1);
    CHECK_EQ(a.Touch.size(), (size_t)1);
    CHECK_EQ(a.Touch[0].Offset, 0u);
    CHECK_EQ(a.Touch[0].X, (uint16_t)30);
    CHECK_EQ(a.Touch[0].Y, (uint16_t)40);
    CHECK_EQ(a.Touch[0].Down, (uint8_t)1);
    CHECK_EQ(a.MicKind, (uint8_t)0);
    CHECK(a.Mic.empty());
}

TEST(InputPayloadWithoutTouchCarriesPen)
{
    InputQueue q;
    InputPayload p = Pay(1, 0);
    p.Touch = {{0, 7, 8, 1}};
    Put(q, p);
    Put(q, Pay(2, 0x1));        // no touch points: no change
    AppliedInput a;
    q.Next(0, a);
    q.Next(1, a);
    CHECK(a.Consumed);
    CHECK_EQ(a.Touch.size(), (size_t)1);
    CHECK_EQ(a.Touch[0].X, (uint16_t)7);
    CHECK_EQ(a.Touch[0].Down, (uint8_t)1);
}

TEST(InputStallReleasesClientAfterTimeout)
{
    InputQueue q;
    q.SetStallTimeout(3);
    InputPayload p = Pay(1, 0xF);
    p.Lid = 1;
    p.Touch = {{0, 5, 5, 1}};
    Put(q, p);
    AppliedInput a;
    q.Next(0, a);
    for (int i = 1; i <= 3; i++)
    {
        q.Next(i, a);
        CHECK_EQ(a.Buttons, (uint16_t)0xF);         // still held for N empty frames
    }
    q.Next(4, a);
    CHECK_EQ(a.Buttons, (uint16_t)0);
    CHECK_EQ(a.Touch.back().Down, (uint8_t)0);
    CHECK_EQ(a.Lid, (uint8_t)1);                    // lid is not released
    q.Next(5, a);
    CHECK_EQ(a.Buttons, (uint16_t)0);
}

TEST(InputStallZeroNeverReleases)
{
    InputQueue q;
    q.SetStallTimeout(0);
    Put(q, Pay(1, 0x3));
    AppliedInput a;
    q.Next(0, a);
    for (int i = 1; i < 500; i++)
    {
        q.Next(i, a);
        CHECK_EQ(a.Buttons, (uint16_t)0x3);
    }
}

TEST(InputLocalSourceNeverStalls)
{
    InputQueue q;
    q.SetStallTimeout(2);
    Put(q, Pay(1, 0x3, InputSource::Local));
    AppliedInput a;
    q.Next(0, a);
    for (int i = 1; i < 100; i++)
    {
        q.Next(i, a);
        CHECK_EQ(a.Buttons, (uint16_t)0x3);
    }
}

TEST(InputNeutralMode)
{
    InputQueue q;
    q.SetMode(EmptyMode::Neutral);
    InputPayload p = Pay(1, 0x3);
    p.Lid = 1;
    p.Touch = {{0, 5, 5, 1}};
    Put(q, p);
    AppliedInput a;
    q.Next(0, a);
    CHECK_EQ(a.Buttons, (uint16_t)3);
    q.Next(1, a);
    CHECK(!a.Consumed);
    CHECK_EQ(a.Buttons, (uint16_t)0);
    CHECK_EQ(a.Touch.back().Down, (uint8_t)0);
    CHECK_EQ(a.Lid, (uint8_t)1);
    CHECK_EQ(a.MicKind, (uint8_t)0);
}

TEST(InputWaitModeDoesNotConsume)
{
    InputQueue q;
    q.SetMode(EmptyMode::Wait);
    AppliedInput a;
    CHECK(!q.Next(0, a));
    CHECK(!q.Next(0, a));
    CHECK_EQ(q.Depth(), (size_t)0);
    Put(q, Pay(1, 1));
    CHECK(q.Next(0, a));
    CHECK(a.Consumed);
    CHECK(!q.Next(1, a));
    CHECK(q.ReadLog(0, 100).size() == 1);
}

TEST(InputModeChangeAppliesAtNextFrame)
{
    InputQueue q;
    q.SetMode(EmptyMode::Wait);
    AppliedInput a;
    CHECK(!q.Next(0, a));
    q.SetMode(EmptyMode::Repeat);
    CHECK(q.Next(0, a));
    CHECK(!a.Consumed);
    q.SetMode(EmptyMode::Wait);
    CHECK(!q.Next(1, a));
    CHECK(q.Mode() == EmptyMode::Wait);
}

TEST(InputLogRecordsFrameAndSeq)
{
    InputQueue q;
    Put(q, Pay(11, 0x1));
    Put(q, Pay(12, 0x2, InputSource::Local));
    Put(q, Pay(13, 0x4));
    AppliedInput a;
    q.Next(100, a);
    q.Next(101, a);
    q.Next(102, a);
    q.Next(103, a);     // empty: not logged
    auto log = q.ReadLog(0, 100);
    CHECK_EQ(log.size(), (size_t)3);
    CHECK_EQ(log[0].Frame, (uint64_t)100);
    CHECK_EQ(log[0].Seq, 11u);
    CHECK_EQ(log[1].Seq, 0u);           // Local
    CHECK_EQ(log[2].Buttons, (uint16_t)4);
    CHECK_EQ(q.ReadLog(101, 100).size(), (size_t)2);
    CHECK_EQ(q.ReadLog(101, 1).size(), (size_t)1);
    CHECK_EQ(q.ReadLog(101, 1)[0].Frame, (uint64_t)101);
    CHECK_EQ(q.ReadLog(500, 10).size(), (size_t)0);
}

TEST(InputLogKeepsFullPayloads)
{
    InputQueue q;
    std::string err;
    InputPayload p;
    p.Seq = 5;
    p.Buttons = 0x81;
    p.Lid = 1;
    p.Touch = {{0, 10, 20, 1}, {1000, 11, 21, 0}};
    p.MicKind = 1;
    p.Mic = {1, -2, 3};
    CHECK(q.Submit(p, err) >= 0);
    InputPayload quiet;
    quiet.Seq = 6;
    quiet.MicKind = 2;
    quiet.MicLevel = 900;
    CHECK(q.Submit(quiet, err) >= 0);
    AppliedInput a;
    q.Next(0, a);
    q.Next(1, a);
    auto log = q.ReadLog(0, 10);
    CHECK_EQ(log.size(), (size_t)2);
    CHECK_EQ(log[0].Touch.size(), (size_t)2);
    CHECK_EQ(log[0].Touch[1].Offset, 1000u);
    CHECK_EQ(log[0].Touch[1].Down, (uint8_t)0);
    CHECK_EQ(log[0].Mic.size(), (size_t)3);
    CHECK_EQ(log[0].Mic[1], (int16_t)-2);
    CHECK_EQ(log[1].Touch.size(), (size_t)0);       // "no change" stays "no change" in the log
    CHECK_EQ(log[1].MicLevel, (uint16_t)900);
}

TEST(InputLogMemoryIsBounded)
{
    InputQueue q;
    AppliedInput a;
    std::string err;
    for (uint64_t f = 0; f < 30000; f++)
    {
        InputPayload p;
        p.Seq = (uint32_t)f + 1;
        p.MicKind = 1;
        p.Mic.assign(InputMaxMicSamples, 7);
        CHECK(q.Submit(p, err) >= 0);
        q.Next(f, a);
    }
    auto log = q.ReadLog(0, 100000);
    CHECK(log.size() < 30000);                      // 30000 x ~1.6 KB is over the log byte budget
    CHECK(log.size() > 1000);
    CHECK_EQ(log.back().Frame, (uint64_t)29999);    // the most recent entries are the ones kept
}

TEST(InputLogKeepsMostRecent)
{
    InputQueue q;
    AppliedInput a;
    for (uint64_t f = 0; f < InputLogCapacity + 10; f++)
    {
        Put(q, Pay((uint32_t)f + 1, 1));
        q.Next(f, a);
    }
    auto log = q.ReadLog(0, InputLogCapacity + 100);
    CHECK_EQ(log.size(), InputLogCapacity);
    CHECK_EQ(log.front().Frame, (uint64_t)10);
}

TEST(InputClearAndReset)
{
    InputQueue q;
    Put(q, Pay(1, 1));
    Put(q, Pay(2, 2));
    AppliedInput a;
    q.Next(0, a);
    Put(q, Pay(3, 4));
    q.Clear();
    CHECK_EQ(q.Depth(), (size_t)0);
    CHECK_EQ(q.ReadLog(0, 10).size(), (size_t)1);       // Clear keeps the log
    q.Next(1, a);
    CHECK_EQ(a.Buttons, (uint16_t)1);                   // and the held state
    q.Reset();
    CHECK_EQ(q.ReadLog(0, 10).size(), (size_t)0);
    q.Next(0, a);
    CHECK_EQ(a.Buttons, (uint16_t)0);
}

TEST(InputWakeOnSubmitAndModeChange)
{
    InputQueue q;
    int wakes = 0;
    q.SetWake([&] { wakes++; });
    Put(q, Pay(1, 1));
    q.SetMode(EmptyMode::Wait);
    q.Clear();
    CHECK_EQ(wakes, 3);
}

TEST(InputModifyAmendsNewestSetter)
{
    InputQueue q;
    std::string err;
    CHECK_EQ(q.Modify([](InputPayload& p) { p.Buttons = 0x1; }, err), 1);
    CHECK_EQ(q.Modify([](InputPayload& p) { p.Touch = {{0, 10, 10, 1}}; }, err), 1);   // amended
    AppliedInput a;
    q.Next(0, a);
    CHECK_EQ(a.Buttons, (uint16_t)1);               // the key survives the touch amend
    CHECK_EQ(a.Touch.back().X, (uint16_t)10);
    CHECK(a.Source == InputSource::Local);

    // A non-setter payload in between stops the coalescing, and the newest state carries over.
    q.Modify([](InputPayload& p) { p.Buttons = 0x2; }, err);
    Put(q, Pay(5, 0x8));
    CHECK_EQ(q.Modify([](InputPayload& p) { p.Lid = 1; }, err), 3);
    q.Next(1, a);
    q.Next(2, a);
    q.Next(3, a);
    CHECK_EQ(a.Buttons, (uint16_t)0x8);
    CHECK_EQ(a.Lid, (uint8_t)1);
    CHECK_EQ(a.Touch.back().X, (uint16_t)10);
}

// What a savestate load restores: the empty-queue rule repeats the restored state, and a client's stall
// countdown continues where it was.
TEST(InputHeldStateRestoresTheRepeatedInput)
{
    InputQueue a;
    Put(a, Pay(1, 0x5, InputSource::Client));
    AppliedInput in;
    CHECK(a.Next(0, in));
    CHECK(a.Next(1, in));       // one empty frame
    InputQueue::Held held = a.GetHeld();
    CHECK_EQ(held.Last.Buttons, (uint16_t)0x5);
    CHECK_EQ(held.EmptyFrames, 1);

    InputQueue b;
    Put(b, Pay(2, 0x2, InputSource::Local));
    CHECK(b.Next(0, in));
    b.SetHeld(held);
    b.SetStallTimeout(3);
    CHECK(b.Next(1, in));       // empty frame 2 of 3: the restored state, still held
    CHECK(!in.Consumed);
    CHECK_EQ(in.Buttons, (uint16_t)0x5);
    CHECK(in.Source == InputSource::Client);
    CHECK(b.Next(2, in));       // 3 of 3
    CHECK_EQ(in.Buttons, (uint16_t)0x5);
    CHECK(b.Next(3, in));       // the fourth releases
    CHECK_EQ(in.Buttons, (uint16_t)0);
}
