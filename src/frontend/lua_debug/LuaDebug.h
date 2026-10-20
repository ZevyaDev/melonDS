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
#include <memory>
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
    std::function<bool(const std::function<void(melonDS::NDS&)>&)> Exec;
    std::function<void()> Wait;
    std::function<void()> Resume;
    // If set and it returns a message, the debugger functions (memory hooks, call tracking, coverage, trace,
    // last writer, pause, step) raise that message as a Lua error instead of doing nothing.
    std::function<std::string()> DebugUnavailable;
};

void Register(lua_State* L, Host host);
void Unregister(lua_State* L);
void RunFrameEnd(lua_State* L);
std::function<void(bool)> FrameHook(lua_State* L, void (*fn)(lua_State*, bool));
int CPUArg(lua_State* L, int idx, int def);

class Guard
{
public:
    explicit Guard(lua_State* L);
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    std::shared_ptr<void> Hold;
    std::recursive_mutex* Lock;
};

}

#endif
