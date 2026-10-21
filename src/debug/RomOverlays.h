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

#ifndef ROMOVERLAYS_H
#define ROMOVERLAYS_H

#include <functional>
#include <vector>

#include "../types.h"

namespace melonDS
{
class NDS;
struct NDSHeader;

struct RomOverlay
{
    int Id;
    u32 Base;
    u32 Size;              // code + bss
    std::vector<u8> Image; // decompressed
};

// RomOverlays.cpp holds the parts that work on plain bytes, without a console, so that programs which do not link
// the emulator (debug_common_tests) can use them. The overloads taking an NDS are in RomOverlaysNDS.cpp.

// Decompresses a BLZ (backwards LZ) overlay image in place; false if it is malformed.
bool BlzDecompress(std::vector<u8>& data);
// The ARM9 overlays of a ROM image, decompressed.
std::vector<RomOverlay> ReadRomOverlays(const u8* rom, u32 len, const NDSHeader& header);
// Whether image (an overlay of size bytes at base) is resident, judged by sampling memory through peek32,
// a 32-bit ARM9 read without side effects.
bool OverlayLoaded(const std::function<u32(u32)>& peek32, u32 base, u32 size, const std::vector<u8>& image);

// The overlays of the cart in nds (empty without a ROM).
std::vector<RomOverlay> ReadRomOverlays(const NDS& nds);
bool OverlayLoaded(const NDS& nds, u32 base, u32 size, const std::vector<u8>& image);

}

#endif
