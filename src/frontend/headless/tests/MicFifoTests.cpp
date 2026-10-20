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

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "TestHarness.h"
#include "MicFifo.h"

using namespace Headless;

#define CHECK_EQ(a, b) ::TestHarness::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

static std::vector<int16_t> Seq(int from, size_t n)
{
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = int16_t(from + int(i));
    return v;
}

TEST(MicFifoOrderAndPartialRead)
{
    MicFifo f;
    auto a = Seq(1, 5), b = Seq(100, 3);
    f.Push(a.data(), a.size());
    f.Push(b.data(), b.size());
    CHECK_EQ(f.Size(), size_t(8));

    int16_t out[16] = {};
    CHECK_EQ(f.Read(out, 3), size_t(3));
    CHECK_EQ(int(out[0]), 1);
    CHECK_EQ(int(out[2]), 3);
    CHECK_EQ(f.Read(out, 16), size_t(5));
    CHECK_EQ(int(out[0]), 4);
    CHECK_EQ(int(out[1]), 5);
    CHECK_EQ(int(out[2]), 100);
    CHECK_EQ(int(out[4]), 102);
    CHECK_EQ(f.Size(), size_t(0));
}

TEST(MicFifoEmptyReadReturnsZero)
{
    MicFifo f;
    int16_t out[4] = {7, 7, 7, 7};
    CHECK_EQ(f.Read(out, 4), size_t(0));
    CHECK_EQ(int(out[0]), 7);
    CHECK_EQ(f.Read(out, 0), size_t(0));
}

TEST(MicFifoCapDropsOldest)
{
    MicFifo f;
    auto a = Seq(0, MicFifo::Capacity);
    f.Push(a.data(), a.size());
    CHECK_EQ(f.Size(), MicFifo::Capacity);
    auto b = Seq(10000, 10);
    f.Push(b.data(), b.size());
    CHECK_EQ(f.Size(), MicFifo::Capacity);

    std::vector<int16_t> out(MicFifo::Capacity);
    CHECK_EQ(f.Read(out.data(), out.size()), MicFifo::Capacity);
    CHECK_EQ(int(out[0]), 10);                      // the ten oldest went
    CHECK_EQ(int(out[MicFifo::Capacity - 11]), int(MicFifo::Capacity - 1));
    CHECK_EQ(int(out[MicFifo::Capacity - 10]), 10000);
    CHECK_EQ(int(out[MicFifo::Capacity - 1]), 10009);
}

TEST(MicFifoOversizedPushKeepsNewest)
{
    MicFifo f;
    auto a = Seq(0, MicFifo::Capacity + 100);
    f.Push(a.data(), a.size());
    CHECK_EQ(f.Size(), MicFifo::Capacity);
    int16_t out[1];
    f.Read(out, 1);
    CHECK_EQ(int(out[0]), 100);
}

TEST(MicFifoCapacityIsFourFrames)
{
    CHECK_EQ(MicFifo::Capacity, size_t(4 * 796));
}

TEST(MicFifoClearAndActive)
{
    MicFifo f;
    CHECK_EQ(int(f.Active()), 0);
    auto a = Seq(1, 10);
    f.Push(a.data(), a.size());
    f.Clear();
    CHECK_EQ(f.Size(), size_t(0));

    f.Start();
    CHECK_EQ(int(f.Active()), 1);
    f.Push(a.data(), a.size());
    f.Stop();                                       // stopping empties the FIFO too
    CHECK_EQ(int(f.Active()), 0);
    CHECK_EQ(f.Size(), size_t(0));
}

TEST(MicNoiseDeterministicPerFrame)
{
    std::vector<int16_t> a, b, c;
    GenerateMicNoise(1234, 40000, a);
    GenerateMicNoise(1234, 40000, b);
    GenerateMicNoise(1235, 40000, c);
    CHECK_EQ(int(a == b), 1);
    CHECK_EQ(int(a == c), 0);
    CHECK_EQ(int(a.size() >= 795 && a.size() <= 796), 1);
}

TEST(MicNoiseFrameZeroIsNotDegenerate)
{
    std::vector<int16_t> a;
    GenerateMicNoise(0, 65535, a);
    int nonzero = 0;
    for (int16_t s : a)
        nonzero += s != 0;
    CHECK_EQ(int(nonzero > 700), 1);
}

TEST(MicNoiseAmplitudeBounded)
{
    for (int level : {1, 2, 100, 4096, 30000, 65535})
    {
        int lim = level / 2;
        int peak = 0;
        for (uint64_t frame = 0; frame < 20; frame++)
        {
            std::vector<int16_t> v;
            GenerateMicNoise(frame, uint16_t(level), v);
            for (int16_t s : v)
                peak = std::max(peak, std::abs(int(s)));
        }
        CHECK_EQ(int(peak <= lim), 1);
        if (level >= 100)
            CHECK_EQ(int(peak > lim * 9 / 10), 1);  // fills most of the range
    }
}

TEST(MicNoiseLevelZeroIsSilent)
{
    std::vector<int16_t> v;
    GenerateMicNoise(77, 0, v);
    CHECK_EQ(int(v.size() >= 795), 1);
    for (int16_t s : v)
        CHECK_EQ(int(s), 0);
}

TEST(MicNoiseFramesTrackTheMicRate)
{
    // 560190 / 704 = 795.7 samples per frame on average, 795 or 796 each
    size_t total = 0;
    for (uint64_t frame = 0; frame < 1000; frame++)
    {
        size_t n = MicNoiseSamplesForFrame(frame);
        CHECK_EQ(int(n == 795 || n == 796), 1);
        total += n;
    }
    CHECK_EQ(total, size_t(1000ull * 560190 / 704));
}
