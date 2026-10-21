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

#include <cstring>

#include "DebugSymbols.h"
#include "NDS.h"
#include "NDSCart.h"

using namespace melonDS;

// The parts of the symbol table that read a console (its cart, its memory). They are in their own file so that a program using
// only the rest (debug_common_tests) does not link the emulator core.

DebugSymbols::RomOverlays DebugSymbols::readRomOverlays(NDS* nds)
{
    RomOverlays out;
    NDSCart::CartCommon* cart = nds ? nds->GetNDSCart() : nullptr;
    if (!cart || !cart->GetROM())
        return out;
    const u8* rom = cart->GetROM();
    u32 len = cart->GetROMLength();
    const NDSHeader& h = cart->GetHeader();
    auto word = [&](u32 off) {
        u32 v = 0;
        if ((u64)off + 4 <= len)
            memcpy(&v, rom + off, 4);
        return v;
    };
    for (u32 i = 0; i < h.ARM9OverlaySize / 32; i++)
    {
        u32 at = h.ARM9OverlayOffset + i * 32;
        if ((u64)at + 32 > len)
            break;
        u32 fat = h.FATOffset + word(at + 24) * 8;
        u32 start = word(fat), end = word(fat + 4);
        if (end <= start || end > len)
            continue;
        std::vector<u8> image(rom + start, rom + end);
        u32 flags = word(at + 28);
        if ((flags >> 24) & 1)
        {
            u32 packed = flags & 0xFFFFFF;
            if (packed && packed < (u32)image.size())
                image.resize(packed);
            if (!blzDecompress(image))
                continue;
        }
        out[(int)word(at)] = {word(at + 4), word(at + 8) + word(at + 12), std::move(image)};
    }
    return out;
}

bool DebugSymbols::load(const std::string& rawPath, std::string& error, NDS* nds, const Files* files)
{
    RomOverlays rom = readRomOverlays(nds);
    return load(rawPath, error, &rom, files);
}

std::vector<int> DebugSymbols::detect(const Set& set, NDS& nds)
{
    return detect(set, [&nds](u32 addr) -> u32 { return nds.Debugger.Peek(0, addr, 4); });
}

