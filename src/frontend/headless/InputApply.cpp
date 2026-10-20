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

// KeyInput is not part of savestates, so keys are applied every frame, not only on change.
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

// Plan step 3 replaces this with the sub-frame timeline; for now only the pen state at the end
// of the frame is applied, at the start of it.
void ApplyTouch(NDS& nds, const AppliedInput& in)
{
    if (in.Touch.empty())
        return;
    const TouchPoint& t = in.Touch.back();
    if (t.Down)
        nds.TouchScreen(t.X, t.Y);
    else
        nds.ReleaseScreen();
}

// Plan step 4.
void ApplyMic(Instance& inst, const AppliedInput& in)
{
}

void ApplyInput(Instance& inst, const AppliedInput& in)
{
    ApplyButtons(*inst.NDS, in);
    ApplyLid(*inst.NDS, in);
    ApplyTouch(*inst.NDS, in);
    ApplyMic(inst, in);
}

}
