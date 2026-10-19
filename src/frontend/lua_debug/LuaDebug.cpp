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
#include <map>
#include <vector>

#include "LuaDebug.h"
#include "NDS.h"

using namespace melonDS;

namespace LuaDebug
{

struct Hook
{
    int ID;
    int Ref;
    std::string Name;
    bool Frame;
};

struct Context
{
    Host H;
    lua_State* L;
    std::vector<Hook> Hooks;
    int NextFrameID = -1;
    std::recursive_mutex Lock;
};

static const char* ContextKey = "melonDS.LuaDebug";

static Context* Ctx(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, ContextKey);
    auto ctx = static_cast<Context*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ctx;
}

static melonDS::NDS& Console(lua_State* L)
{
    melonDS::NDS* nds = Ctx(L)->H.GetNDS();
    if (!nds)
        luaL_error(L, "no console running");
    return *nds;
}

int CPUArg(lua_State* L, int idx, int def)
{
    if (lua_isnoneornil(L, idx))
        return def;
    if (lua_isnumber(L, idx))
    {
        int n = (int)lua_tointeger(L, idx);
        if (n == 7 || n == 1)
            return 1;
        if (n == 9 || n == 0)
            return 0;
        return luaL_argerror(L, idx, "cpu must be 9 or 7");
    }
    std::string s = luaL_checkstring(L, idx);
    if (s.rfind("ARM7", 0) == 0 || s == "arm7")
        return 1;
    if (s.rfind("ARM9", 0) == 0 || s == "arm9" || s == "Main RAM" || s == "System Bus")
        return 0;
    return luaL_argerror(L, idx, "unknown domain");
}

static void PushEvent(lua_State* L, const BreakEvent& ev)
{
    lua_pushinteger(L, ev.Addr);
    lua_pushinteger(L, ev.Value);
    lua_pushinteger(L, ev.PC);
}

static bool CallHook(Context* ctx, int ref, const BreakEvent* ev)
{
    std::lock_guard lock(ctx->Lock);
    lua_State* L = ctx->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    int nargs = 0;
    if (ev)
    {
        PushEvent(L, *ev);
        nargs = 3;
    }
    if (lua_pcall(L, nargs, 1, 0) != LUA_OK)
    {
        ctx->H.Print(std::string("lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    bool brk = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return brk;
}

static int AddHook(lua_State* L, BreakKind kind)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    u32 addr = (u32)luaL_checkinteger(L, 2);
    std::string name = luaL_optstring(L, 3, "");
    int cpu = CPUArg(L, 4, 0);
    u32 size = (u32)luaL_optinteger(L, 5, 1);

    Context* ctx = Ctx(L);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    BreakCallback cb = [ctx, ref](const BreakEvent& ev) { return CallHook(ctx, ref, &ev); };

    melonDS::NDS& nds = Console(L);
    int id;
    if (kind == BreakKind::Exec)
        id = nds.Debugger.AddBreakpoint(1 << cpu, addr, cb);
    else
        id = nds.Debugger.AddWatchpoint(1 << cpu, addr, size, kind == BreakKind::Read ? Watch_Read : Watch_Write, cb);
    if (id < 0)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "cannot add hook");
    }
    ctx->Hooks.push_back({id, ref, name, false});
    lua_pushinteger(L, id);
    return 1;
}

static int OnExecute(lua_State* L) { return AddHook(L, BreakKind::Exec); }
static int OnRead(lua_State* L) { return AddHook(L, BreakKind::Read); }
static int OnWrite(lua_State* L) { return AddHook(L, BreakKind::Write); }

static int OnFrameEnd(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Context* ctx = Ctx(L);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int id = ctx->NextFrameID--;
    ctx->Hooks.push_back({id, ref, luaL_optstring(L, 2, ""), true});
    lua_pushinteger(L, id);
    return 1;
}

static void DropHook(lua_State* L, Context* ctx, size_t i)
{
    Hook h = ctx->Hooks[i];
    ctx->Hooks.erase(ctx->Hooks.begin() + i);
    if (!h.Frame)
        if (melonDS::NDS* nds = ctx->H.GetNDS())
            nds->Debugger.Remove(h.ID);
    luaL_unref(L, LUA_REGISTRYINDEX, h.Ref);
}

static int UnregisterByID(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    Context* ctx = Ctx(L);
    for (size_t i = 0; i < ctx->Hooks.size(); i++)
        if (ctx->Hooks[i].ID == id)
        {
            DropHook(L, ctx, i);
            lua_pushboolean(L, 1);
            return 1;
        }
    lua_pushboolean(L, 0);
    return 1;
}

static int UnregisterByName(lua_State* L)
{
    std::string name = luaL_checkstring(L, 1);
    Context* ctx = Ctx(L);
    bool any = false;
    for (size_t i = ctx->Hooks.size(); i-- > 0;)
        if (ctx->Hooks[i].Name == name)
        {
            DropHook(L, ctx, i);
            any = true;
        }
    lua_pushboolean(L, any);
    return 1;
}

static const char* const RegNames[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
};

static ARM& CPU(melonDS::NDS& nds, int cpu)
{
    return cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
}

static u32 InstrAddress(melonDS::NDS& nds, int cpu)
{
    if (nds.Debugger.IsBreaking())
        return nds.Debugger.CurPC[cpu];
    ARM& arm = CPU(nds, cpu);
    return arm.R[15] - ((arm.CPSR & 0x20) ? 2 : 4);
}

static int ParseRegister(const std::string& full, int& cpu)
{
    std::string name = full;
    if (name.rfind("ARM7 ", 0) == 0) { cpu = 1; name = name.substr(5); }
    else if (name.rfind("ARM9 ", 0) == 0) { cpu = 0; name = name.substr(5); }
    for (auto& c : name)
        c = tolower(c);
    if (name == "cpsr")
        return 16;
    if (name == "r13") return 13;
    if (name == "r14") return 14;
    if (name == "r15") return 15;
    for (int i = 0; i < 16; i++)
        if (name == RegNames[i])
            return i;
    return -1;
}

static int GetRegister(lua_State* L)
{
    int cpu = CPUArg(L, 2, 0);
    int reg = ParseRegister(luaL_checkstring(L, 1), cpu);
    if (reg < 0)
        return luaL_argerror(L, 1, "unknown register");
    melonDS::NDS& nds = Console(L);
    ARM& arm = CPU(nds, cpu);
    if (reg == 16)
        lua_pushinteger(L, arm.CPSR);
    else if (reg == 15)
        lua_pushinteger(L, InstrAddress(nds, cpu));
    else
        lua_pushinteger(L, arm.R[reg]);
    return 1;
}

static int GetRegisters(lua_State* L)
{
    melonDS::NDS& nds = Console(L);
    lua_newtable(L);
    for (int cpu = 0; cpu < 2; cpu++)
    {
        ARM& arm = CPU(nds, cpu);
        const char* prefix = cpu ? "ARM7 " : "ARM9 ";
        for (int i = 0; i < 16; i++)
        {
            lua_pushinteger(L, i == 15 ? InstrAddress(nds, cpu) : arm.R[i]);
            lua_setfield(L, -2, (std::string(prefix) + RegNames[i]).c_str());
        }
        lua_pushinteger(L, arm.CPSR);
        lua_setfield(L, -2, (std::string(prefix) + "cpsr").c_str());
    }
    return 1;
}

static int SetRegister(lua_State* L)
{
    int cpu = CPUArg(L, 3, 0);
    int reg = ParseRegister(luaL_checkstring(L, 1), cpu);
    u32 value = (u32)luaL_checkinteger(L, 2);
    if (reg < 0 || reg == 16)
        return luaL_argerror(L, 1, "register not writable");
    ARM& arm = CPU(Console(L), cpu);
    if (reg == 15)
        arm.JumpTo(value | ((arm.CPSR & 0x20) ? 1 : 0));
    else
        arm.R[reg] = value;
    return 0;
}

static int CallStack(lua_State* L)
{
    int cpu = CPUArg(L, 1, 0);
    auto& stack = Console(L).Debugger.GetCallStack(cpu);
    lua_createtable(L, stack.size(), 0);
    for (size_t i = 0; i < stack.size(); i++)
    {
        const CallFrame& f = stack[stack.size() - 1 - i];
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, f.Entry); lua_setfield(L, -2, "entry");
        lua_pushinteger(L, f.Site); lua_setfield(L, -2, "site");
        lua_pushinteger(L, f.Return); lua_setfield(L, -2, "ret");
        lua_pushboolean(L, f.Exception); lua_setfield(L, -2, "exception");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int TrackCalls(lua_State* L)
{
    Console(L).Debugger.SetCallTracking(lua_toboolean(L, 1));
    return 0;
}

static int CallCounts(lua_State* L)
{
    int cpu = CPUArg(L, 1, 0);
    auto counts = Console(L).Debugger.DrainCallCounts(cpu);
    lua_createtable(L, 0, counts.size());
    for (auto& [entry, n] : counts)
    {
        lua_pushinteger(L, n);
        lua_rawseti(L, -2, entry);
    }
    return 1;
}

static int Coverage(lua_State* L)
{
    Console(L).Debugger.SetCoverage(lua_toboolean(L, 1));
    return 0;
}

static int DrainCoverage(lua_State* L)
{
    auto addrs = Console(L).Debugger.DrainCoverage(CPUArg(L, 1, 0));
    lua_createtable(L, addrs.size(), 0);
    for (size_t i = 0; i < addrs.size(); i++)
    {
        lua_pushinteger(L, addrs[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int Trace(lua_State* L)
{
    u32 capacity = (u32)luaL_optinteger(L, 1, 65536);
    int cpu = CPUArg(L, 2, 0);
    u32 start = (u32)luaL_optinteger(L, 3, 0);
    u32 end = (u32)luaL_optinteger(L, 4, 0xFFFFFFFF);
    Console(L).Debugger.SetTrace(1 << cpu, start, end, capacity);
    return 0;
}

static int DrainTrace(lua_State* L)
{
    auto entries = Console(L).Debugger.DrainTrace();
    lua_createtable(L, entries.size(), 0);
    for (size_t i = 0; i < entries.size(); i++)
    {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, entries[i].PC); lua_setfield(L, -2, "pc");
        lua_pushinteger(L, entries[i].Instr); lua_setfield(L, -2, "instr");
        lua_pushinteger(L, entries[i].CPU ? 7 : 9); lua_setfield(L, -2, "cpu");
        lua_pushinteger(L, entries[i].Timestamp); lua_setfield(L, -2, "cycle");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int TrackWriters(lua_State* L)
{
    Console(L).Debugger.SetLastWriter(lua_toboolean(L, 1));
    return 0;
}

static int LastWriter(lua_State* L)
{
    u32 w = Console(L).Debugger.GetLastWriter((u32)luaL_checkinteger(L, 1));
    if (!w)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, w & ~1u);
    lua_pushinteger(L, (w & 1) ? 7 : 9);
    return 2;
}

static int Break(lua_State* L)
{
    Console(L).Debugger.RequestBreak(1 << CPUArg(L, 1, 0));
    return 0;
}

static int Step(lua_State* L)
{
    Console(L).Debugger.Step(CPUArg(L, 2, 0), (u32)luaL_optinteger(L, 1, 1));
    return 0;
}

static int Peek(lua_State* L)
{
    u32 addr = (u32)luaL_checkinteger(L, 1);
    int size = (int)luaL_optinteger(L, 2, 4);
    if (size != 1 && size != 2 && size != 4)
        return luaL_argerror(L, 2, "size must be 1, 2 or 4");
    lua_pushinteger(L, Console(L).Debugger.Peek(CPUArg(L, 3, 0), addr, size));
    return 1;
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

void Register(lua_State* L, Host host)
{
    auto ctx = new Context {std::move(host), L};
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, ContextKey);

    static const luaL_Reg eventFuncs[] = {
        {"onmemoryexecute", OnExecute},
        {"onmemoryread", OnRead},
        {"onmemorywrite", OnWrite},
        {"onframeend", OnFrameEnd},
        {"unregisterbyid", UnregisterByID},
        {"unregisterbyname", UnregisterByName},
        {nullptr, nullptr},
    };
    static const luaL_Reg emuFuncs[] = {
        {"getregister", GetRegister},
        {"getregisters", GetRegisters},
        {"setregister", SetRegister},
        {nullptr, nullptr},
    };
    static const luaL_Reg debugFuncs[] = {
        {"callstack", CallStack},
        {"trackcalls", TrackCalls},
        {"callcounts", CallCounts},
        {"coverage", Coverage},
        {"draincoverage", DrainCoverage},
        {"trace", Trace},
        {"draintrace", DrainTrace},
        {"trackwriters", TrackWriters},
        {"lastwriter", LastWriter},
        {"pause", Break},
        {"step", Step},
        {"peek", Peek},
        {nullptr, nullptr},
    };
    SetFuncs(L, "event", eventFuncs);
    SetFuncs(L, "emu", emuFuncs);
    SetFuncs(L, "debugger", debugFuncs);
}

void Unregister(lua_State* L)
{
    Context* ctx = Ctx(L);
    if (!ctx)
        return;
    while (!ctx->Hooks.empty())
        DropHook(L, ctx, ctx->Hooks.size() - 1);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, ContextKey);
    delete ctx;
}

Guard::Guard(lua_State* L) : Lock(nullptr)
{
    if (Context* ctx = Ctx(L))
    {
        Lock = &ctx->Lock;
        Lock->lock();
    }
}

Guard::~Guard()
{
    if (Lock)
        Lock->unlock();
}

void RunFrameEnd(lua_State* L)
{
    Context* ctx = Ctx(L);
    if (!ctx)
        return;
    std::lock_guard lock(ctx->Lock);
    std::vector<int> refs;
    for (auto& h : ctx->Hooks)
        if (h.Frame)
            refs.push_back(h.Ref);
    for (int ref : refs)
        CallHook(ctx, ref, nullptr);
}

}
