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

#include <vector>

#include "../types.h"

namespace melonDS
{
class NDS;

struct RomOverlay
{
    int Id;
    u32 Base;
    u32 Size;
    std::vector<u8> Image;
};

bool BlzDecompress(std::vector<u8>& data);
std::vector<RomOverlay> ReadRomOverlays(const NDS& nds);
bool OverlayLoaded(const NDS& nds, u32 base, u32 size, const std::vector<u8>& image);

}

#endif
