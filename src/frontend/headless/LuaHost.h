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

#ifndef HEADLESS_LUAHOST_H
#define HEADLESS_LUAHOST_H

#include <mutex>
#include <string>

#include <lua.hpp>

#include "Input.h"

namespace Headless
{

class Instance;

class LuaHost
{
public:
    explicit LuaHost(Instance& inst);
    ~LuaHost();

    bool Run(const std::string& source, bool isFile, std::string& error);
    bool Exec(const std::string& code, std::string& error);
    void Stop();
    std::string TakeOutput();

    void OnFrameEnd();
    void OnReset();

    void Print(const std::string& text);

    // Input set by the script is gathered here and submitted as one payload when the script
    // yields (or the hook or exec returns).
    InputState& EditInput();
    InputState CurrentInput() const;
    void FlushInput();

    Instance& Inst;

private:
    lua_State* L = nullptr;
    lua_State* Main = nullptr;
    int MainRef = LUA_NOREF;
    InputState Pending;
    bool PendingActive = false;
    std::mutex OutputLock;
    std::string Output;

    void Open();
    void Close();
    void Resume();
};

}

#endif
