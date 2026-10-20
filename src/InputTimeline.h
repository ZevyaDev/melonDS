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

#ifndef INPUTTIMELINE_H
#define INPUTTIMELINE_H

#include "types.h"

namespace melonDS
{

// Optional source of sub-frame touch input. When one is set on an NDS, the touch controller
// asks it for the pen state whenever the ARM7 samples it (SPI conversion commands and reads of
// EXTKEYIN), instead of relying on the single state stored by NDS::TouchScreen.
class InputTimeline
{
public:
    virtual ~InputTimeline() = default;

    // The pen state at the given time, in system (33.51 MHz) cycles since the start of the
    // current frame. Returns true if the pen is down, with the screen position in x (0..255)
    // and y (0..191). Called from the emulation thread; must not touch the NDS.
    virtual bool TouchAt(u64 cyclesSinceFrameStart, u16& x, u16& y) = 0;
};

}

#endif
