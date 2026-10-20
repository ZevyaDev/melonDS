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
#include <string>

#include "TestHarness.h"
#include "Barrier.h"
#include "Input.h"

using namespace Headless;

TEST(BarrierReleasesWhenEveryoneArrived)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1, 2});
    CHECK(id != 0);
    CHECK_EQ(g.GroupCount(), (size_t)1);
    g.Arrive(id, 0);
    CHECK(!g.Released(id, 0));
    g.Arrive(id, 2);
    CHECK(!g.Released(id, 0));
    CHECK(!g.Released(id, 2));
    g.Arrive(id, 1);
    CHECK(g.Released(id, 0));
    CHECK(g.Released(id, 1));
    CHECK(g.Released(id, 2));
}

TEST(BarrierIdsAreDistinctAndNonzero)
{
    BarrierGroups g;
    uint32_t a = g.Create({0, 1});
    uint32_t b = g.Create({0, 1});
    CHECK(a != 0 && b != 0 && a != b);
    CHECK_EQ(g.GroupCount(), (size_t)2);
}

TEST(BarrierStaysReleasedWhileMembersApply)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1});
    g.Arrive(id, 0);
    g.Arrive(id, 1);
    g.Applied(id, 0);
    CHECK(g.Released(id, 1));       // the partner that applied first must not re-block the other
    CHECK_EQ(g.GroupCount(), (size_t)1);
    g.Applied(id, 1);
    CHECK_EQ(g.GroupCount(), (size_t)0);
    CHECK(g.Released(id, 1));       // a finished group never blocks
}

TEST(BarrierUnknownGroupDoesNotBlock)
{
    BarrierGroups g;
    CHECK(g.Released(12345, 0));
}

TEST(BarrierMemberDropReleasesTheOthers)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1, 2});
    g.Arrive(id, 0);
    g.Arrive(id, 1);
    CHECK(!g.Released(id, 0));
    g.Drop(id, 2);
    CHECK(g.Released(id, 0));
    CHECK(g.Released(id, 1));
    CHECK(g.Released(id, 2));       // the dropped member itself proceeds when it gets there
}

TEST(BarrierDropConsoleHitsEveryGroup)
{
    BarrierGroups g;
    uint32_t a = g.Create({0, 1});
    uint32_t b = g.Create({1, 2});
    g.Arrive(a, 0);
    g.Arrive(b, 2);
    g.DropConsole(1);
    CHECK(g.Released(a, 0));
    CHECK(g.Released(b, 2));
}

TEST(BarrierGroupGoneWhenEveryoneAppliedOrLeft)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1});
    g.Arrive(id, 0);
    g.Drop(id, 1);
    CHECK_EQ(g.GroupCount(), (size_t)1);
    g.Applied(id, 0);
    CHECK_EQ(g.GroupCount(), (size_t)0);
}

TEST(BarrierCancelRemovesTheGroup)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1});
    g.Cancel(id);
    CHECK_EQ(g.GroupCount(), (size_t)0);
    CHECK(g.Released(id, 0));
}

TEST(BarrierWirelessReceivePendingReleasesAWaiter)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1});
    g.Arrive(id, 0);
    CHECK(!g.Released(id, 0));
    g.SetRecvPending(1, true);          // console 1 is blocked in a wireless receive
    CHECK(g.Released(id, 0));
    g.SetRecvPending(1, false);
    CHECK(!g.Released(id, 0));
}

TEST(BarrierOwnOrForeignReceiveDoesNotRelease)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1});
    g.Arrive(id, 0);
    g.SetRecvPending(0, true);          // its own receive: it is waiting, not receiving
    CHECK(!g.Released(id, 0));
    g.SetRecvPending(0, false);
    g.SetRecvPending(5, true);          // a console outside the group
    CHECK(!g.Released(id, 0));
    g.SetRecvPending(5, false);
}

TEST(BarrierWirelessReceiveCountsNest)
{
    BarrierGroups g;
    uint32_t id = g.Create({0, 1});
    g.Arrive(id, 0);
    g.SetRecvPending(1, true);
    g.SetRecvPending(1, true);
    g.SetRecvPending(1, false);
    CHECK(g.Released(id, 0));
    g.SetRecvPending(1, false);
    CHECK(!g.Released(id, 0));
}

TEST(BarrierWakesWaitingMembers)
{
    BarrierGroups g;
    std::atomic<int> wake[3] {};
    for (int i = 0; i < 3; i++)
        g.Register(i, [&wake, i] { wake[i]++; });
    uint32_t id = g.Create({0, 1, 2});

    g.Arrive(id, 0);                    // nobody else is waiting yet
    CHECK_EQ((int)wake[1], 0);
    g.Arrive(id, 1);                    // 0 is waiting and may now be able to go
    CHECK_EQ((int)wake[0], 1);
    CHECK_EQ((int)wake[1], 0);
    g.DropConsole(2);                   // a drop wakes the arrived members
    CHECK_EQ((int)wake[0], 2);
    CHECK_EQ((int)wake[1], 1);
    g.SetRecvPending(2, true);          // gone member: no longer relevant
    CHECK_EQ((int)wake[0], 2);

    g.Unregister(0);
    g.Drop(id, 1);
    CHECK_EQ((int)wake[0], 2);          // an unregistered console is not called
    for (int i = 0; i < 3; i++)
        g.Unregister(i);
}

TEST(BarrierWakesOnWirelessReceive)
{
    BarrierGroups g;
    std::atomic<int> wake0 {0};
    g.Register(0, [&wake0] { wake0++; });
    uint32_t id = g.Create({0, 1});
    g.Arrive(id, 0);
    g.SetRecvPending(1, true);
    CHECK_EQ((int)wake0, 1);
    g.Unregister(0);
}

// ---- queue integration

static InputPayload Pay(uint32_t seq, uint16_t buttons)
{
    InputPayload p;
    p.Seq = seq;
    p.Buttons = buttons;
    return p;
}

TEST(BarrierQueueSubmitGroupMarksPayloads)
{
    BarrierGroups g;
    InputQueue q0, q1;
    std::vector<GroupEntry> entries {{&q0, 0, Pay(1, 1)}, {&q1, 1, Pay(2, 2)}};
    std::vector<int> depths;
    std::string err;
    CHECK(q0.FrontBarrier() == 0);
    CHECK(SubmitGroup(entries, depths, err, g));
    CHECK_EQ(depths.size(), (size_t)2);
    CHECK_EQ(depths[0], 1);
    CHECK_EQ(depths[1], 1);
    uint32_t id = q0.FrontBarrier();
    CHECK(id != 0);
    CHECK_EQ(q1.FrontBarrier(), id);
    CHECK_EQ(g.GroupCount(), (size_t)1);

    AppliedInput a;
    CHECK(q0.Next(0, a));
    CHECK_EQ(a.Barrier, id);
    CHECK_EQ(q0.FrontBarrier(), (uint32_t)0);
}

TEST(BarrierQueueFrontLooksOnlyAtTheNextPayload)
{
    BarrierGroups g;
    InputQueue q;
    std::string err;
    CHECK(q.Submit(Pay(1, 1), err) == 1);
    std::vector<GroupEntry> entries {{&q, 0, Pay(2, 2)}};
    std::vector<int> depths;
    CHECK(SubmitGroup(entries, depths, err, g));
    CHECK_EQ(q.FrontBarrier(), (uint32_t)0);
    AppliedInput a;
    CHECK(q.Next(0, a));
    CHECK(q.FrontBarrier() != 0);
}

TEST(BarrierQueueSubmitGroupIsAllOrNothing)
{
    BarrierGroups g;
    InputQueue q0, q1;
    std::string err;
    for (size_t i = 0; i < InputQueueCapacity; i++)
        CHECK(q1.Submit(Pay(1, 1), err) > 0);

    std::vector<GroupEntry> entries {{&q0, 0, Pay(1, 1)}, {&q1, 1, Pay(2, 2)}};
    std::vector<int> depths;
    CHECK(!SubmitGroup(entries, depths, err, g));
    CHECK(err == "input_queue_full");
    CHECK_EQ(q0.Depth(), (size_t)0);
    CHECK_EQ(q1.Depth(), InputQueueCapacity);
    CHECK_EQ(g.GroupCount(), (size_t)0);
}

TEST(BarrierQueueSubmitGroupRejectsBadPayloadAndDuplicates)
{
    BarrierGroups g;
    InputQueue q0, q1;
    std::string err;
    InputPayload bad = Pay(2, 2);
    bad.Buttons = 0x8000;
    std::vector<GroupEntry> entries {{&q0, 0, Pay(1, 1)}, {&q1, 1, bad}};
    std::vector<int> depths;
    CHECK(!SubmitGroup(entries, depths, err, g));
    CHECK(err.rfind("bad_payload", 0) == 0);
    CHECK_EQ(q0.Depth(), (size_t)0);
    CHECK_EQ(g.GroupCount(), (size_t)0);

    std::vector<GroupEntry> dup {{&q0, 0, Pay(1, 1)}, {&q0, 0, Pay(2, 2)}};
    CHECK(!SubmitGroup(dup, depths, err, g));
    CHECK(err.rfind("bad_payload", 0) == 0);
    CHECK_EQ(q0.Depth(), (size_t)0);

    std::vector<GroupEntry> none;
    CHECK(!SubmitGroup(none, depths, err, g));
}

TEST(BarrierClearingAQueueDropsItFromTheGroup)
{
    BarrierGroups g;
    InputQueue q0, q1;
    q0.SetBarrierOwner(0, &g);
    q1.SetBarrierOwner(1, &g);
    std::vector<GroupEntry> entries {{&q0, 0, Pay(1, 1)}, {&q1, 1, Pay(2, 2)}};
    std::vector<int> depths;
    std::string err;
    CHECK(SubmitGroup(entries, depths, err, g));
    uint32_t id = q0.FrontBarrier();
    g.Arrive(id, 0);
    CHECK(!g.Released(id, 0));
    q1.Clear();                         // reset / savestate load / disconnect
    CHECK(g.Released(id, 0));
    q0.Reset();
    CHECK_EQ(g.GroupCount(), (size_t)0);
}
