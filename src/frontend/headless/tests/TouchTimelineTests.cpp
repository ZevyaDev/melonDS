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
#include "Input.h"

using namespace Headless;

namespace
{

struct Sample
{
    bool Down;
    int X, Y;
};

Sample At(TouchTimeline& t, uint64_t cycles)
{
    melonDS::u16 x = 0xFFFF, y = 0xFFFF;
    bool down = t.TouchAt(cycles, x, y);
    return {down, down ? x : -1, down ? y : -1};
}

#define CHECK_SAMPLE(t, cycles, down, x, y) \
    do { Sample s_ = At(t, cycles); \
         CHECK_EQ((int)s_.Down, (int)(down)); CHECK_EQ(s_.X, (int)(x)); CHECK_EQ(s_.Y, (int)(y)); } while (0)

}

TEST(TouchTimelineStartsReleased)
{
    TouchTimeline t;
    CHECK_SAMPLE(t, 0, false, -1, -1);
    t.Begin({});
    CHECK_SAMPLE(t, 12345, false, -1, -1);
}

TEST(TouchTimelineOffsetsBeforeAtBetweenAfter)
{
    TouchTimeline t;
    t.Begin({{1000, 10, 20, 1}, {5000, 30, 40, 1}, {9000, 0, 0, 0}});
    CHECK_SAMPLE(t, 0, false, -1, -1);          // before the first point: the previous state
    CHECK_SAMPLE(t, 999, false, -1, -1);
    CHECK_SAMPLE(t, 1000, true, 10, 20);        // at the offset the point already holds
    CHECK_SAMPLE(t, 4999, true, 10, 20);
    CHECK_SAMPLE(t, 5000, true, 30, 40);
    CHECK_SAMPLE(t, 8999, true, 30, 40);
    CHECK_SAMPLE(t, 9000, false, -1, -1);       // release point
    CHECK_SAMPLE(t, 560189, false, -1, -1);
}

TEST(TouchTimelineCarriesIntoLaterFrames)
{
    TouchTimeline t;
    t.Begin({{0, 7, 8, 1}, {3000, 9, 10, 1}});
    CHECK_SAMPLE(t, 560189, true, 9, 10);
    t.Begin({{4000, 1, 2, 1}});
    CHECK_SAMPLE(t, 0, true, 9, 10);            // previous frame's final state until the first point
    CHECK_SAMPLE(t, 3999, true, 9, 10);
    CHECK_SAMPLE(t, 4000, true, 1, 2);
    t.Begin({{100, 0, 0, 0}});
    CHECK_SAMPLE(t, 99, true, 1, 2);
    CHECK_SAMPLE(t, 100, false, -1, -1);
    t.Begin({});
    CHECK_SAMPLE(t, 0, false, -1, -1);          // empty list: previous (released) state
    CHECK_SAMPLE(t, 500000, false, -1, -1);
}

TEST(TouchTimelineEmptyListKeepsPressedState)
{
    TouchTimeline t;
    t.Begin({{0, 100, 50, 1}});
    t.Begin({});
    CHECK_SAMPLE(t, 0, true, 100, 50);
    CHECK_SAMPLE(t, 400000, true, 100, 50);
    t.Begin({});
    CHECK_SAMPLE(t, 400000, true, 100, 50);
}

TEST(TouchTimelineSingleOffsetZeroPoint)
{
    TouchTimeline t;
    t.Begin({{0, 255, 191, 1}});
    CHECK_SAMPLE(t, 0, true, 255, 191);
    CHECK_SAMPLE(t, 560189, true, 255, 191);
}

TEST(TouchTimelineReset)
{
    TouchTimeline t;
    t.Begin({{0, 5, 5, 1}});
    t.Reset();
    t.Begin({});
    CHECK_SAMPLE(t, 0, false, -1, -1);
}
