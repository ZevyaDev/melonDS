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

#ifndef LUADEBUG_H
#define LUADEBUG_H

#include <functional>
#include <mutex>
#include <string>

#include <lua.hpp>

namespace melonDS
{
class NDS;
}

namespace LuaDebug
{

struct Host
{
    std::function<melonDS::NDS*()> GetNDS;
    std::function<void(const std::string&)> Print;
};

void Register(lua_State* L, Host host);
void Unregister(lua_State* L);
void RunFrameEnd(lua_State* L);
int CPUArg(lua_State* L, int idx, int def);

class Guard
{
public:
    explicit Guard(lua_State* L);
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    std::recursive_mutex* Lock;
};

}

#endif
