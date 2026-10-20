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

#include "Input.h"
#include "Instance.h"

using namespace melonDS;

namespace Headless
{

// Keys are applied every frame, not only on change (savestates before 14.2 did not carry KeyInput).
void ApplyButtons(NDS& nds, const AppliedInput& in)
{
    nds.SetKeyMask(~in.Buttons & 0xFFF);
}

void ApplyLid(NDS& nds, const AppliedInput& in)
{
    // SetLidClosed(false) raises the lid-open IRQ every call, so only act on a change.
    bool closed = in.Lid != 0;
    if (nds.IsLidClosed() != closed)
        nds.SetLidClosed(closed);
}

// The points become the frame's timeline, which the core consults whenever the ARM7 samples the
// touch controller. The state at the start of the frame is also applied directly, so anything
// that reads the controller without going through the timeline sees a coherent pen.
void ApplyTouch(NDS& nds, TouchTimeline& line, const AppliedInput& in)
{
    line.Begin(in.Touch);
    uint16_t x, y;
    if (line.StateAt(0, x, y))
        nds.TouchScreen(x, y);
    else
        nds.ReleaseScreen();
}

// Samples go to the console's mic FIFO, which melonDS drains as it wants them. While the mic is
// off (the game has not sampled it, or it timed out) they are dropped. The active flag follows
// the core's own Mic_Start/Mic_Stop calls, including those made by a savestate load.
void ApplyMic(Instance& inst, const AppliedInput& in)
{
    if (!inst.Mic.Active())
        return;

    if (in.MicKind == 1)
    {
        if (!in.Mic.empty())
            inst.Mic.Push(in.Mic.data(), in.Mic.size());
    }
    else if (in.MicKind == 2)
    {
        std::vector<int16_t> noise;
        GenerateMicNoise(inst.EmuFrame, in.MicLevel, noise);
        inst.Mic.Push(noise.data(), noise.size());
    }
}

void ApplyInput(Instance& inst, const AppliedInput& in)
{
    ApplyButtons(*inst.NDS, in);
    ApplyLid(*inst.NDS, in);
    ApplyTouch(*inst.NDS, inst.TouchLine, in);
    ApplyMic(inst, in);
}

}
