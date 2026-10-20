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

#ifndef HEADLESS_MICFIFO_H
#define HEADLESS_MICFIFO_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace Headless
{

// Per-console microphone sample FIFO (design option A). Input::Apply pushes a frame's samples,
// Platform::Mic_ReadInput pulls them as melonDS asks for them. Both run on the emulator thread;
// the lock only makes stray callers safe.
class MicFifo
{
public:
    static constexpr size_t FrameSamples = 796;
    static constexpr size_t Capacity = 4 * FrameSamples;    // four frames; the oldest samples go first

    void Push(const int16_t* samples, size_t count);
    size_t Read(int16_t* dst, size_t max);      // returns how many were copied, 0 when empty
    size_t Size() const;
    void Clear();
    // The queued samples, oldest first, and putting them back (savestates; Instance::DoSavestateExtra).
    std::vector<int16_t> Snapshot() const;
    void Restore(const std::vector<int16_t>& samples);

    // The core's view of the microphone. Stop also empties the FIFO.
    void Start();
    void Stop();
    bool Active() const;

private:
    mutable std::mutex Lock;
    std::deque<int16_t> Samples;
    bool IsActive = false;
};

// Samples that one frame of the 47.6 kHz mic feed (one per 704 cycles) consumes: 795 or 796,
// 795.7 on average, so a stream generated frame by frame does not drift against the core.
size_t MicNoiseSamplesForFrame(uint64_t frame);

// One frame of noise for a level payload: xorshift32 seeded from the frame number alone, so the
// same frame gives the same samples on every console and every replay. The caller passes the console's
// emulated frame (Instance::EmuFrame, which savestates carry), not the process's frame count. Samples are uniform in
// [-level/2, level/2], so level 65535 is full scale and level 0 is silence.
void GenerateMicNoise(uint64_t frame, uint16_t level, std::vector<int16_t>& out);

}

#endif
