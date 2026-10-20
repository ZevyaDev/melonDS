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

#include "MicFifo.h"

namespace Headless
{

void MicFifo::Push(const int16_t* samples, size_t count)
{
    std::lock_guard lock(Lock);
    if (count >= Capacity)
    {
        // only the newest Capacity samples can survive
        samples += count - Capacity;
        count = Capacity;
        Samples.clear();
    }
    else if (Samples.size() + count > Capacity)
        Samples.erase(Samples.begin(), Samples.begin() + (Samples.size() + count - Capacity));
    Samples.insert(Samples.end(), samples, samples + count);
}

size_t MicFifo::Read(int16_t* dst, size_t max)
{
    std::lock_guard lock(Lock);
    size_t n = Samples.size() < max ? Samples.size() : max;
    for (size_t i = 0; i < n; i++)
        dst[i] = Samples[i];
    Samples.erase(Samples.begin(), Samples.begin() + n);
    return n;
}

size_t MicFifo::Size() const
{
    std::lock_guard lock(Lock);
    return Samples.size();
}

void MicFifo::Clear()
{
    std::lock_guard lock(Lock);
    Samples.clear();
}

void MicFifo::Start()
{
    std::lock_guard lock(Lock);
    IsActive = true;
    Samples.clear();
}

void MicFifo::Stop()
{
    std::lock_guard lock(Lock);
    IsActive = false;
    Samples.clear();
}

bool MicFifo::Active() const
{
    std::lock_guard lock(Lock);
    return IsActive;
}

size_t MicNoiseSamplesForFrame(uint64_t frame)
{
    constexpr uint64_t FrameCycles = 560190, SampleCycles = 704;
    return size_t(((frame + 1) * FrameCycles) / SampleCycles - (frame * FrameCycles) / SampleCycles);
}

void GenerateMicNoise(uint64_t frame, uint16_t level, std::vector<int16_t>& out)
{
    uint32_t x = uint32_t(frame) * 2654435761u + 0x9E3779B9u;
    if (!x)
        x = 1;

    size_t n = MicNoiseSamplesForFrame(frame);
    out.resize(n);
    for (size_t i = 0; i < n; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        int32_t r = int16_t(x >> 16);                   // -32768 .. 32767
        out[i] = int16_t(r * int32_t(level) / 65536);   // truncates toward zero: |sample| <= level/2
    }
}

}
