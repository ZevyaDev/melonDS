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

#include "DebugSymbols.h"
#include "NDS.h"
#include "debug/RomOverlays.h"

using namespace melonDS;

// The parts of the symbol table that read a console (its cart, its memory). They are in their own file so that a program using
// only the rest (debug_common_tests) does not link the emulator core.

DebugSymbols::RomOverlays DebugSymbols::readRomOverlays(NDS* nds)
{
    RomOverlays out;
    if (nds)
        for (RomOverlay& ov : ReadRomOverlays(*nds))
            out[ov.Id] = std::move(ov);
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
