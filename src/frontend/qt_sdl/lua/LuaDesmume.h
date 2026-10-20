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

#ifndef LUADESMUME_H
#define LUADESMUME_H

#include <QString>

#include <lua.hpp>

class EmuInstance;

namespace LuaDesmume
{

void Register(lua_State* L);
bool Start(lua_State* L, const char* file, QString& error);
void Frame(lua_State* L, bool after);
void Close(lua_State* L);
void UpdateHostInput(EmuInstance* inst);

}

#endif
