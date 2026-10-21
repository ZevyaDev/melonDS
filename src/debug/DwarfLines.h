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

#ifndef DWARFLINES_H
#define DWARFLINES_H

#include <string>
#include <vector>

#include "../types.h"

namespace melonDS
{

struct DwarfLineRow
{
    u32 Addr;
    u32 Line;
    u32 File;
    bool Stmt;
    bool End;
};

struct DwarfLineProgram
{
    std::vector<std::string> Files;
    std::vector<DwarfLineRow> Rows;
};

bool ReadDwarfLines(const u8* elf, size_t len, std::vector<DwarfLineProgram>& out, std::string& error);

}

#endif
