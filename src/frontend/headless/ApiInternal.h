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

#ifndef HEADLESS_APIINTERNAL_H
#define HEADLESS_APIINTERNAL_H

// What the socket server needs from the console registry in Api.cpp. Everything the C API
// already does (mds_*) is reused through melonds_api.h; this adds only what it cannot express.

#include <cstdint>
#include <functional>
#include <vector>

namespace Headless
{

class Instance;

namespace Api
{

// The console with this id, or null. The pointer stays valid until that console is destroyed.
Instance* FindInstance(int id);

// Ids of every console, ascending.
std::vector<int> ConsoleIds();

// Called after mds_create has registered a console / after mds_destroy has torn one down
// (its emulator thread is gone). Either may be empty. Called on the creating/destroying thread.
void SetConsoleHooks(std::function<void(Instance&)> created, std::function<void(int)> destroyed);

// Copy guest memory as the given CPU sees it (cpu 0/9 = ARM9, 1/7 = ARM7). Must run on the
// console's emulator thread (Instance::Call).
void ReadGuest(Instance& inst, int cpu, uint32_t addr, uint32_t len, uint8_t* out);

}

}

#endif
