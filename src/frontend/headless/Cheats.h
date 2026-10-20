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

#ifndef HEADLESS_CHEATS_H
#define HEADLESS_CHEATS_H

// Action Replay code text for the headless API (mds_cheat_*). Pure logic, no NDS: the codes themselves run
// in the core's AREngine, which a console's cheat list feeds (Instance::SyncCheats).

#include <cstdint>
#include <string>
#include <vector>

namespace Headless
{

// Parse Action Replay code text into the words the core's engine runs (ARCode::Code): one
// "XXXXXXXX YYYYYYYY" pair of hex words per line (case-insensitive, any spaces or tabs around and between
// them, CR LF or LF line ends, blank lines ignored). The code is checked as the engine will read it: every
// opcode must be one the engine runs (melonDS's AREngine stops at an unknown opcode and at C4, which it does
// not support), a D4 data operation must be 00-08, and an E (copy) opcode must be followed by the data lines
// it copies. On failure returns false with *error naming the line ("line 3: ...").
bool ParseCheatCode(const std::string& text, std::vector<uint32_t>& words, std::string& error);

// Longest name and code accepted by mds_cheat_add.
constexpr size_t MaxCheatName = 4096;
constexpr size_t MaxCheatWords = 2 * 4096;

}

#endif
