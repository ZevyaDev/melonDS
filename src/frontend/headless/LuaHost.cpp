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

#include <algorithm>
#include <cstring>

#include "LuaHost.h"
#include "Instance.h"
#include "LuaDebug.h"

using namespace melonDS;

namespace Headless
{

static const char* HostKey = "melonDS.Headless";

static LuaHost& Host(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, HostKey);
    auto host = static_cast<LuaHost*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *host;
}

static melonDS::NDS& Console(lua_State* L)
{
    return *Host(L).Inst.NDS;
}

static const char* const KeyNames[] = {
    "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y",
};

static u32 DomainAddress(lua_State* L, int idx, u32 addr, int& cpu)
{
    cpu = 0;
    if (lua_isnoneornil(L, idx))
        return addr;
    std::string domain = luaL_checkstring(L, idx);
    if (domain == "Main RAM")
        return 0x02000000 + addr;
    cpu = LuaDebug::CPUArg(L, idx, 0);
    return addr;
}

template <int size, bool sign>
static int ReadValue(lua_State* L)
{
    int cpu;
    u32 addr = DomainAddress(L, 2, (u32)luaL_checkinteger(L, 1), cpu);
    melonDS::NDS& nds = Console(L);
    u32 v = 0;
    for (int i = 0; i < size; i++)
        v |= nds.Debugger.Peek(cpu, addr + i, 1) << (8 * i);
    if (sign)
        lua_pushinteger(L, (s32)(v << (32 - 8 * size)) >> (32 - 8 * size));
    else
        lua_pushinteger(L, v);
    return 1;
}

template <int size>
static int WriteValue(lua_State* L)
{
    int cpu;
    u32 addr = DomainAddress(L, 3, (u32)luaL_checkinteger(L, 1), cpu);
    u32 v = (u32)luaL_checkinteger(L, 2);
    melonDS::NDS& nds = Console(L);
    for (int i = 0; i < size; i++)
        nds.Debugger.Poke(cpu, addr + i, 1, (v >> (8 * i)) & 0xFF);
    return 0;
}

static int ReadBytes(lua_State* L)
{
    int cpu;
    u32 addr = DomainAddress(L, 3, (u32)luaL_checkinteger(L, 1), cpu);
    lua_Integer len = luaL_checkinteger(L, 2);
    melonDS::NDS& nds = Console(L);
    lua_createtable(L, len, 0);
    for (lua_Integer i = 0; i < len; i++)
    {
        lua_pushinteger(L, nds.Debugger.Peek(cpu, addr + i, 1));
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int WriteBytes(lua_State* L)
{
    int cpu;
    u32 addr = DomainAddress(L, 3, (u32)luaL_checkinteger(L, 1), cpu);
    luaL_checktype(L, 2, LUA_TTABLE);
    melonDS::NDS& nds = Console(L);
    lua_Integer len = luaL_len(L, 2);
    for (lua_Integer i = 0; i < len; i++)
    {
        lua_rawgeti(L, 2, i + 1);
        nds.Debugger.Poke(cpu, addr + i, 1, (u32)lua_tointeger(L, -1) & 0xFF);
        lua_pop(L, 1);
    }
    return 0;
}

static int FrameCount(lua_State* L)
{
    lua_pushinteger(L, Host(L).Inst.FramesRun);
    return 1;
}

static int FrameAdvance(lua_State* L)
{
    if (!lua_isyieldable(L))
        return luaL_error(L, "emu.frameadvance() only works in the main script body");
    return lua_yield(L, 0);
}

static int GetSystemID(lua_State* L)
{
    lua_pushstring(L, Console(L).ConsoleType ? "DSi" : "NDS");
    return 1;
}

static int GetConsoleID(lua_State* L)
{
    lua_pushinteger(L, Host(L).Inst.EffectiveID());
    return 1;
}

static int JoypadSet(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    InputState& state = Host(L).EditInput();
    for (int i = 0; i < 12; i++)
    {
        lua_getfield(L, 1, KeyNames[i]);
        if (!lua_isnil(L, -1))
        {
            if (lua_toboolean(L, -1))
                state.Buttons |= 1u << i;
            else
                state.Buttons &= ~(1u << i);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int JoypadGet(lua_State* L)
{
    InputState state = Host(L).CurrentInput();
    lua_createtable(L, 0, 12);
    for (int i = 0; i < 12; i++)
    {
        lua_pushboolean(L, (state.Buttons >> i) & 1);
        lua_setfield(L, -2, KeyNames[i]);
    }
    return 1;
}

static int TouchDown(lua_State* L)
{
    InputState& state = Host(L).EditInput();
    state.TouchDown = 1;
    state.TouchX = (u16)std::clamp<lua_Integer>(luaL_checkinteger(L, 1), 0, 255);
    state.TouchY = (u16)std::clamp<lua_Integer>(luaL_checkinteger(L, 2), 0, 191);
    return 0;
}

static int TouchUp(lua_State* L)
{
    Host(L).EditInput().TouchDown = 0;
    return 0;
}

static int SaveState(lua_State* L)
{
    std::string error;
    if (!Host(L).Inst.SaveState(luaL_checkstring(L, 1), error))
        return luaL_error(L, "%s", error.c_str());
    return 0;
}

static int LoadState(lua_State* L)
{
    std::string error;
    if (!Host(L).Inst.LoadState(luaL_checkstring(L, 1), error))
        return luaL_error(L, "%s", error.c_str());
    return 0;
}

static int LuaPrint(lua_State* L)
{
    std::string line;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++)
    {
        if (i > 1)
            line += '\t';
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    Host(L).Print(line);
    return 0;
}

static void SetFuncs(lua_State* L, const char* table, const luaL_Reg* funcs)
{
    lua_getglobal(L, table);
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    luaL_setfuncs(L, funcs, 0);
    lua_setglobal(L, table);
}

// The error on top of the stack as text; never empty, so an empty text still reads as a failure.
static std::string ErrorText(lua_State* L)
{
    const char* text = lua_tostring(L, -1);
    if (!text)
        return "(error object is not a string)";
    return *text ? text : "(empty error message)";
}

LuaHost::LuaHost(Instance& inst) : Inst(inst)
{
    Open();
}

LuaHost::~LuaHost()
{
    Close();
}

void LuaHost::Open()
{
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, HostKey);
    lua_register(L, "print", LuaPrint);

    static const luaL_Reg memoryFuncs[] = {
        {"read_u8", ReadValue<1, false>},
        {"read_s8", ReadValue<1, true>},
        {"read_u16_le", ReadValue<2, false>},
        {"read_s16_le", ReadValue<2, true>},
        {"read_u24_le", ReadValue<3, false>},
        {"read_s24_le", ReadValue<3, true>},
        {"read_u32_le", ReadValue<4, false>},
        {"read_s32_le", ReadValue<4, true>},
        {"readbyte", ReadValue<1, false>},
        {"write_u8", WriteValue<1>},
        {"write_s8", WriteValue<1>},
        {"write_u16_le", WriteValue<2>},
        {"write_s16_le", WriteValue<2>},
        {"write_u24_le", WriteValue<3>},
        {"write_s24_le", WriteValue<3>},
        {"write_u32_le", WriteValue<4>},
        {"write_s32_le", WriteValue<4>},
        {"writebyte", WriteValue<1>},
        {"read_bytes_as_array", ReadBytes},
        {"write_bytes_as_array", WriteBytes},
        {nullptr, nullptr},
    };
    static const luaL_Reg emuFuncs[] = {
        {"framecount", FrameCount},
        {"frameadvance", FrameAdvance},
        {"getsystemid", GetSystemID},
        {"getconsoleid", GetConsoleID},
        {nullptr, nullptr},
    };
    static const luaL_Reg joypadFuncs[] = {
        {"set", JoypadSet},
        {"get", JoypadGet},
        {nullptr, nullptr},
    };
    static const luaL_Reg inputFuncs[] = {
        {"NDSTapDown", TouchDown},
        {"NDSTapUp", TouchUp},
        {nullptr, nullptr},
    };
    static const luaL_Reg savestateFuncs[] = {
        {"save", SaveState},
        {"load", LoadState},
        {nullptr, nullptr},
    };
    SetFuncs(L, "memory", memoryFuncs);
    SetFuncs(L, "emu", emuFuncs);
    SetFuncs(L, "joypad", joypadFuncs);
    SetFuncs(L, "input", inputFuncs);
    SetFuncs(L, "savestate", savestateFuncs);

    LuaDebug::Host host;
    host.GetNDS = [this] { return Inst.NDS.get(); };
    host.Print = [this](const std::string& s) { Print(s); };
    // a console created with debug = 0 runs without the debugger hooks: the debugger functions fail
    host.DebugUnavailable = [this]() -> std::string {
        if (Inst.DebugEnabled())
            return "";
        return "console " + std::to_string(Inst.ID) + " was created without the debugger (debug = 0)";
    };
    LuaDebug::Register(L, std::move(host));
}

void LuaHost::Close()
{
    if (!L)
        return;
    LuaDebug::Unregister(L);
    lua_close(L);
    L = nullptr;
    Main = nullptr;
    MainRef = LUA_NOREF;
}

void LuaHost::Stop()
{
    Close();
    Open();
}

bool LuaHost::Run(const std::string& source, bool isFile, std::string& error)
{
    Stop();
    Main = lua_newthread(L);
    MainRef = luaL_ref(L, LUA_REGISTRYINDEX);
    int status = isFile ? luaL_loadfile(Main, source.c_str()) : luaL_loadbuffer(Main, source.data(), source.size(), "=script");
    if (status != LUA_OK)
    {
        error = ErrorText(Main);
        Stop();
        return false;
    }
    // The body runs until its first emu.frameadvance() or its end; an error there fails the call.
    std::string failure;
    Resume(&failure);
    FlushInput();
    if (!failure.empty())
    {
        error = failure;
        return false;
    }
    return true;
}

bool LuaHost::Exec(const std::string& code, std::string& error)
{
    if (luaL_loadbuffer(L, code.data(), code.size(), "=exec") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK)
    {
        error = ErrorText(L);
        lua_pop(L, 1);
        return false;
    }
    FlushInput();
    return true;
}

void LuaHost::Resume(std::string* error)
{
    if (!Main)
        return;
    int nres = 0;
    int status = lua_resume(Main, L, 0, &nres);
    if (status == LUA_YIELD)
    {
        lua_pop(Main, nres);
        return;
    }
    if (status != LUA_OK)
    {
        std::string message = ErrorText(Main);
        if (error)
            *error = message;
        else
            Print("lua: " + message);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, MainRef);
    MainRef = LUA_NOREF;
    Main = nullptr;
}

void LuaHost::OnFrameEnd()
{
    if (!L)
        return;
    LuaDebug::RunFrameEnd(L);
    Resume();
    FlushInput();
}

void LuaHost::OnReset()
{
    PendingActive = false;
}

InputState& LuaHost::EditInput()
{
    if (!PendingActive)
    {
        Pending = Inst.Input.NewestState();
        PendingActive = true;
    }
    return Pending;
}

InputState LuaHost::CurrentInput() const
{
    return PendingActive ? Pending : Inst.Input.LastState();
}

void LuaHost::FlushInput()
{
    if (!PendingActive)
        return;
    PendingActive = false;
    InputPayload p;
    p.Source = InputSource::Local;
    p.Buttons = Pending.Buttons;
    p.Lid = Pending.Lid;
    p.Touch = {{0, Pending.TouchX, Pending.TouchY, Pending.TouchDown}};
    std::string error;
    if (Inst.Input.Submit(std::move(p), error) < 0)
        Print("lua: input: " + error);
}

void LuaHost::Print(const std::string& text)
{
    std::lock_guard lock(OutputLock);
    Output += text;
    Output += '\n';
}

std::string LuaHost::TakeOutput()
{
    std::lock_guard lock(OutputLock);
    std::string out;
    out.swap(Output);
    return out;
}

}
