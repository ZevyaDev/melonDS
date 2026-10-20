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
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "melonds_api.h"
#include "ApiInternal.h"
#include "Instance.h"
#include "LuaHost.h"
#include "MPInterface.h"
#include "Server.h"

using namespace melonDS;
using namespace Headless;

namespace
{

constexpr int MaxInstances = 16;

struct Slot
{
    std::unique_ptr<Instance> Inst;
    std::vector<TraceEntry> Trace;
    std::vector<std::pair<u32, u64>> Calls[2];
    std::vector<u32> Coverage[2];
    std::string LuaOutput;
    std::vector<PacketRecord> Packets;
};

std::mutex SlotsLock;
std::map<int, Slot> Slots;
thread_local std::string LastError;
bool MPReady = false;

std::mutex HookLock;
std::function<void(Instance&)> CreatedHook;
std::function<void(int)> DestroyedHook;

int Fail(const std::string& error)
{
    LastError = error;
    return -1;
}

Slot* Find(int id)
{
    std::lock_guard lock(SlotsLock);
    auto it = Slots.find(id);
    if (it == Slots.end())
    {
        LastError = "no console " + std::to_string(id);
        return nullptr;
    }
    return &it->second;
}

Slot* Stopped(int id)
{
    Slot* s = Find(id);
    if (s && s->Inst->GetState() == RunState::Running)
    {
        LastError = "console " + std::to_string(id) + " is running";
        return nullptr;
    }
    return s;
}

int StateCode(RunState st)
{
    switch (st)
    {
    case RunState::Running: return MDS_RUNNING;
    case RunState::Parked: return MDS_PARKED;
    default: return MDS_IDLE;
    }
}

ARM& CPU(Instance& inst, int cpu)
{
    return cpu ? (ARM&)*inst.NDS->ARM7 : (ARM&)*inst.NDS->ARM9;
}

u32 SPSR(ARM& arm)
{
    switch (arm.CPSR & 0x1F)
    {
    case 0x11: return arm.R_FIQ[7];
    case 0x12: return arm.R_IRQ[2];
    case 0x13: return arm.R_SVC[2];
    case 0x17: return arm.R_ABT[2];
    case 0x1B: return arm.R_UND[2];
    default: return 0;
    }
}

// Run f(inst, error) on the console's emulator thread and return its result. A negative
// result with a non-empty error reports that error to the caller's thread.
template <typename F>
int OnEmu(Slot* s, F f)
{
    int result = -1;
    std::string error;
    if (!s->Inst->Call([&](Instance& inst) { result = f(inst, error); }))
        return Fail("console is shutting down");
    if (result < 0 && !error.empty())
        LastError = error;
    return result;
}

template <typename T, typename F>
int Take(std::vector<T>& staging, F refill, T* out, int max)
{
    if (staging.empty())
        staging = refill();
    int n = std::min<int>(max, staging.size());
    std::copy(staging.begin(), staging.begin() + n, out);
    staging.erase(staging.begin(), staging.begin() + n);
    return n;
}

}

extern "C" {

int mds_version(void)
{
    return 1;
}

const char* mds_error(void)
{
    return LastError.c_str();
}

void mds_set_local_dir(const char* dir)
{
    SetLocalDirectory(dir ? dir : ".");
}

void mds_set_log_level(int level)
{
    SetLogThreshold((Platform::LogLevel)level);
}

void mds_set_pacing(int fps)
{
    SetPacing(fps);
}

int mds_mp_timeout(int ms)
{
    if (!MPReady)
        return Fail("create a console first");
    if (ms >= 0)
        MPInterface::Get().SetRecvTimeout(ms);
    return MPInterface::Get().GetRecvTimeout();
}

int mds_create(const char* bios9, const char* bios7, const char* firmware, const char* nickname, int debug)
{
    int id = -1;
    {
        std::lock_guard lock(SlotsLock);
        for (int i = 0; i < MaxInstances; i++)
            if (!Slots.count(i))
            {
                id = i;
                break;
            }
        if (id < 0)
            return Fail("too many consoles");
        if (!MPReady)
        {
            MPInterface::Set(MPInterface_Local);
            MPReady = true;
        }
    }

    Config config;
    config.BIOS9Path = bios9 ? bios9 : "";
    config.BIOS7Path = bios7 ? bios7 : "";
    config.FirmwarePath = firmware ? firmware : "";
    config.Nickname = nickname ? nickname : "";
    config.Debug = debug != 0;

    std::string error;
    auto inst = std::make_unique<Instance>(id, config, error);
    if (!error.empty())
        return Fail(error);

    Instance* created = inst.get();
    {
        std::lock_guard lock(SlotsLock);
        Slots[id].Inst = std::move(inst);
    }
    std::function<void(Instance&)> hook;
    {
        std::lock_guard lock(HookLock);
        hook = CreatedHook;
    }
    if (hook)
        hook(*created);
    return id;
}

void mds_destroy(int id)
{
    std::unique_ptr<Instance> inst;
    {
        std::lock_guard lock(SlotsLock);
        auto it = Slots.find(id);
        if (it == Slots.end())
            return;
        inst = std::move(it->second.Inst);
        Slots.erase(it);
    }
    inst.reset();
    std::function<void(int)> hook;
    {
        std::lock_guard lock(HookLock);
        hook = DestroyedHook;
    }
    if (hook)
        hook(id);
}

int mds_load_rom(int id, const char* rom, const char* save)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.LoadROM(rom, save ? save : "", error) ? 0 : -1;
    });
}

int mds_reset(int id)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (s->Inst->GetState() != RunState::Idle)
        return Fail("reset needs an idle console");
    return OnEmu(s, [](Instance& inst, std::string&) {
        inst.Reset();
        return 0;
    });
}

int mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states)
{
    std::vector<Slot*> slots;
    for (int i = 0; i < count; i++)
    {
        Slot* s = Stopped(ids[i]);
        if (!s)
            return -1;
        slots.push_back(s);
    }
    for (Slot* s : slots)
        s->Inst->Start(frames);
    return mds_wait(ids, count, timeout_ms, states);
}

int mds_wait(const int* ids, int count, int timeout_ms, int* states)
{
    std::vector<Slot*> slots;
    for (int i = 0; i < count; i++)
    {
        Slot* s = Find(ids[i]);
        if (!s)
            return -1;
        slots.push_back(s);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool interrupted = false;
    int result = 0;
    for (;;)
    {
        bool running = false, parked = false;
        Slot* waitOn = nullptr;
        for (Slot* s : slots)
        {
            RunState st = s->Inst->GetState();
            if (st == RunState::Running)
            {
                running = true;
                waitOn = s;
            }
            else if (st == RunState::Parked)
                parked = true;
        }
        if (parked && running && !interrupted)
        {
            for (Slot* s : slots)
                if (s->Inst->GetState() == RunState::Running)
                    s->Inst->Interrupt();
            interrupted = true;
        }
        if (!running)
            break;
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
        {
            result = 1;
            break;
        }
        waitOn->Inst->Wait(5);
    }
    if (states)
        for (int i = 0; i < count; i++)
            states[i] = StateCode(slots[i]->Inst->GetState());
    return result;
}

int mds_gdb(int id, int port9, int port7, int break_on_start)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
#ifdef GDBSTUB_ENABLED
    if (port9 <= 0 && port7 <= 0)
        s->Inst->NDS->SetGdbArgs(std::nullopt);
    else
        s->Inst->NDS->SetGdbArgs(GDBArgs{(u16)port7, (u16)port9, break_on_start != 0, break_on_start != 0});
    return 0;
#else
    return Fail("built without the GDB stub");
#endif
}

int mds_start(int id, uint32_t frames)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    s->Inst->Start(frames);
    return 0;
}

int mds_stop_after(int id, uint32_t frames)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Inst->StopAfter(frames);
    return 0;
}

int mds_interrupt(int id)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Inst->Interrupt();
    return 0;
}

int mds_state(int id)
{
    Slot* s = Find(id);
    return s ? StateCode(s->Inst->GetState()) : -1;
}

int mds_take_break(int id, mds_break* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    BreakEvent ev;
    if (!s->Inst->TakeBreak(ev))
        return 0;
    *out = {(int32_t)ev.Kind, ev.CPU ? 7 : 9, ev.Size, ev.ID, ev.PC, ev.Addr, ev.Value,
            ev.DMAChannel == 0xFF ? -1 : (int32_t)ev.DMAChannel};
    return 1;
}

uint64_t mds_frames(int id)
{
    Slot* s = Find(id);
    return s ? s->Inst->FramesRun : 0;
}

int mds_screen(int id, uint32_t* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        void* top = nullptr;
        void* bottom = nullptr;
        if (!inst.NDS->GPU.GetFramebuffers(&top, &bottom) || !top || !bottom)
        {
            error = "no framebuffer";
            return -1;
        }
        memcpy(out, top, 256 * 192 * 4);
        memcpy(out + 256 * 192, bottom, 256 * 192 * 4);
        return 0;
    });
}

// The setters go through the same queue as every other source: they amend the newest queued
// setter payload if there is one, so keys and touch set before a frame land in that frame.
static int Modify(Slot* s, const std::function<void(InputPayload&)>& fn)
{
    std::string error;
    if (s->Inst->Input.Modify(fn, error) < 0)
        return Fail(error);
    return 0;
}

int mds_set_keys(int id, uint32_t pressed)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return Modify(s, [&](InputPayload& p) { p.Buttons = pressed & 0xFFF; });
}

int mds_touch(int id, int x, int y)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return Modify(s, [&](InputPayload& p) {
        if (x < 0 || y < 0)
            p.Touch = {{0, 0, 0, 0}};
        else
            p.Touch = {{0, (u16)std::min(x, 255), (u16)std::min(y, 191), 1}};
    });
}

int mds_set_lid(int id, int closed)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return Modify(s, [&](InputPayload& p) { p.Lid = closed != 0; });
}

int mds_input_submit(int id, const mds_input* in, const mds_touch_point* points, const int16_t* mic)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (!in)
        return Fail("bad_payload: no input");
    if ((in->touch_count && !points) || (in->mic_count && !mic))
        return Fail("bad_payload: missing touch or microphone data");
    InputPayload p;
    p.Source = (in->flags & 1) ? InputSource::Client : InputSource::Local;
    p.Seq = in->seq;
    p.Buttons = in->buttons;
    p.Lid = in->lid != 0;
    for (int i = 0; i < in->touch_count; i++)
        p.Touch.push_back({points[i].offset, points[i].x, points[i].y, points[i].down});
    p.MicKind = in->mic_kind;
    if (in->mic_count)
        p.Mic.assign(mic, mic + in->mic_count);
    p.MicLevel = in->mic_level;
    std::string error;
    int depth = s->Inst->Input.Submit(std::move(p), error);
    if (depth < 0)
        return Fail(error);
    return depth;
}

int mds_input_mode(int id, int mode, int stall_frames)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (mode < -1 || mode > 2)
        return Fail("unknown input mode");
    if (stall_frames >= 0)
        s->Inst->Input.SetStallTimeout(stall_frames);
    if (mode >= 0)
        s->Inst->Input.SetMode((EmptyMode)mode);
    return 0;
}

int mds_input_depth(int id)
{
    Slot* s = Find(id);
    return s ? (int)s->Inst->Input.Depth() : -1;
}

int mds_input_waiting(int id)
{
    Slot* s = Find(id);
    return s ? (s->Inst->WaitingForInput() ? 1 : 0) : -1;
}

int mds_input_clear(int id)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Inst->Input.Clear();
    return 0;
}

int mds_input_log(int id, uint64_t from_frame, mds_input_log_entry* out, int max)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    int n = 0;
    for (const InputLogEntry& e : s->Inst->Input.ReadLog(from_frame, max > 0 ? max : 0))
        out[n++] = {e.Frame, e.Seq, e.Buttons, e.Lid, e.TouchCount, e.MicKind, 0, e.MicLevel, 0, 0};
    return n;
}

int mds_read(int id, int cpu, uint32_t addr, uint32_t len, uint8_t* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        Api::ReadGuest(inst, cpu, addr, len, out);
        return 0;
    });
}

// Applied by the emulator thread at the next frame boundary (or at once when idle or parked).
int mds_write(int id, int cpu, uint32_t addr, uint32_t len, const uint8_t* data)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        for (u32 i = 0; i < len; i++)
            if (!inst.NDS->Debugger.Poke(cpu == 7 || cpu == 1, addr + i, 1, data[i]))
            {
                error = "address not writable";
                return -1;
            }
        return 0;
    });
}

int mds_get_regs(int id, int cpu, uint32_t* out)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        int n = cpu == 7 || cpu == 1;
        ARM& arm = CPU(inst, n);
        for (int i = 0; i < 15; i++)
            out[i] = arm.R[i];
        out[15] = inst.GetState() == RunState::Parked ? inst.NDS->Debugger.CurPC[n]
            : arm.R[15] - ((arm.CPSR & 0x20) ? 2 : 4);
        out[16] = arm.CPSR;
        out[17] = SPSR(arm);
        out[18] = arm.R[15];
        return 0;
    });
}

int mds_set_reg(int id, int cpu, int reg, uint32_t value)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        ARM& arm = CPU(inst, cpu == 7 || cpu == 1);
        if (reg < 0 || reg > 15)
        {
            error = "register out of range";
            return -1;
        }
        if (reg == 15)
            arm.JumpTo(value | ((arm.CPSR & 0x20) ? 1 : 0));
        else
            arm.R[reg] = value;
        return 0;
    });
}

int mds_bp_add(int id, uint32_t cpumask, uint32_t addr)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        return inst.NDS->Debugger.AddBreakpoint(cpumask & 3, addr);
    });
}

int mds_wp_add(int id, uint32_t cpumask, uint32_t addr, uint32_t size, uint32_t flags)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        int r = inst.NDS->Debugger.AddWatchpoint(cpumask & 3, addr, size, flags & 3);
        if (r < 0)
            error = "bad watchpoint";
        return r;
    });
}

int mds_point_remove(int id, int point)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        if (inst.NDS->Debugger.Remove(point))
            return 0;
        error = "no such point";
        return -1;
    });
}

int mds_point_enable(int id, int point, int enabled)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        if (inst.NDS->Debugger.SetEnabled(point, enabled != 0))
            return 0;
        error = "no such point";
        return -1;
    });
}

int mds_point_list(int id, mds_point* out, int max)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        auto& dbg = inst.NDS->Debugger;
        int n = 0;
        for (auto& b : dbg.GetBreakpoints())
            if (n < max)
                out[n++] = {b.ID, 0, b.CPUMask, 0, b.Enabled, b.Addr, b.Addr, b.Hits};
        for (auto& w : dbg.GetWatchpoints())
            if (n < max)
                out[n++] = {w.ID, 1, w.CPUMask, w.Flags, w.Enabled, w.Start, w.End, w.Hits};
        return n;
    });
}

int mds_step(int id, int cpu, uint32_t count)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.Step(cpu == 7 || cpu == 1, count);
        return 0;
    });
}

int mds_trace(int id, uint32_t cpumask, uint32_t start, uint32_t end, uint32_t capacity)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    s->Trace.clear();
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetTrace(cpumask & 3, start, end, capacity);
        return 0;
    });
}

int mds_trace_take(int id, mds_trace_entry* out, int max)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    std::vector<TraceEntry> buf(max);
    int n = Take(s->Trace, [s] {
        std::vector<TraceEntry> drained;
        s->Inst->Call([&](Instance& inst) { drained = inst.NDS->Debugger.DrainTrace(); });
        return drained;
    }, buf.data(), max);
    for (int i = 0; i < n; i++)
        out[i] = {buf[i].Timestamp, buf[i].PC, buf[i].Instr, buf[i].CPSR, buf[i].CPU ? 7u : 9u};
    return n;
}

int mds_calls(int id, int enabled)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetCallTracking(enabled != 0);
        return 0;
    });
}

int mds_callstack(int id, int cpu, mds_frame* out, int max)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        auto stack = inst.NDS->Debugger.GetCallStack(cpu == 7 || cpu == 1);
        int n = std::min<int>(max, stack.size());
        for (int i = 0; i < n; i++)
        {
            const CallFrame& f = stack[stack.size() - 1 - i];
            out[i] = {f.Site, f.Entry, f.Return, f.SP, f.Mode, f.Exception};
        }
        return n;
    });
}

int mds_callcounts_take(int id, int cpu, uint32_t* entries, uint64_t* counts, int max)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    int n = cpu == 7 || cpu == 1;
    std::vector<std::pair<u32, u64>> buf(max);
    int got = Take(s->Calls[n], [s, n] {
        std::vector<std::pair<u32, u64>> drained;
        s->Inst->Call([&](Instance& inst) {
            auto map = inst.NDS->Debugger.DrainCallCounts(n);
            drained.assign(map.begin(), map.end());
        });
        return drained;
    }, buf.data(), max);
    for (int i = 0; i < got; i++)
    {
        entries[i] = buf[i].first;
        counts[i] = buf[i].second;
    }
    return got;
}

int mds_coverage(int id, int enabled)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetCoverage(enabled != 0);
        return 0;
    });
}

int mds_coverage_take(int id, int cpu, uint32_t* out, int max)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    int n = cpu == 7 || cpu == 1;
    return Take(s->Coverage[n], [s, n] {
        std::vector<u32> drained;
        s->Inst->Call([&](Instance& inst) { drained = inst.NDS->Debugger.DrainCoverage(n); });
        return drained;
    }, out, max);
}

int mds_lastwriter(int id, int enabled)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetLastWriter(enabled != 0);
        return 0;
    });
}

uint32_t mds_lastwriter_get(int id, uint32_t addr)
{
    Slot* s = Stopped(id);
    if (!s)
        return 0;
    uint32_t writer = 0;
    s->Inst->Call([&](Instance& inst) { writer = inst.NDS->Debugger.GetLastWriter(addr); });
    return writer;
}

int mds_savestate(int id, const char* path)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.SaveState(path, error) ? 0 : -1;
    });
}

int mds_loadstate(int id, const char* path)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.LoadState(path, error) ? 0 : -1;
    });
}

int mds_lua_run(int id, const char* source, int is_file)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    std::string error;
    return s->Inst->Lua->Run(source, is_file != 0, error) ? 0 : Fail(error);
}

int mds_lua_exec(int id, const char* code)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    std::string error;
    return s->Inst->Lua->Exec(code, error) ? 0 : Fail(error);
}

int mds_lua_stop(int id)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    s->Inst->Lua->Stop();
    return 0;
}

int mds_lua_output(int id, char* out, int max)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->LuaOutput += s->Inst->Lua->TakeOutput();
    int n = std::min<int>(max, s->LuaOutput.size());
    memcpy(out, s->LuaOutput.data(), n);
    s->LuaOutput.erase(0, n);
    return n;
}

int mds_packets(int id, int enabled, int capacity)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Packets.clear();
    s->Inst->SetPacketLog(enabled != 0, std::max(capacity, 1));
    return 0;
}

int mds_packets_take(int id, mds_packet* headers, uint8_t* data, int max_packets, int max_data)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (s->Packets.empty())
        s->Packets = s->Inst->DrainPackets();
    int n = 0, used = 0;
    while (n < max_packets && n < (int)s->Packets.size())
    {
        auto& p = s->Packets[n];
        if (used + (int)p.Data.size() > max_data)
            break;
        headers[n] = {p.Timestamp, p.Sent, p.Type, (uint32_t)p.Data.size()};
        memcpy(data + used, p.Data.data(), p.Data.size());
        used += p.Data.size();
        n++;
    }
    s->Packets.erase(s->Packets.begin(), s->Packets.begin() + n);
    return n;
}

int mds_serve(int port, int flags, int* bound)
{
    if (port < 0 || port > 65535)
        return Fail("bad port");
    std::string error;
    int actual = 0;
    if (!ServerStart(port, flags & 0xFFFF, actual, error))
        return Fail(error);
    if (bound)
        *bound = actual;
    return 0;
}

void mds_serve_stop(void)
{
    ServerStop();
}

int mds_serve_wait(int timeout_ms)
{
    return ServerWait(timeout_ms);
}

void mds_serve_name(const char* name)
{
    ServerSetName(name ? name : "");
}

int mds_set_instance_base(int base)
{
    {
        std::lock_guard lock(SlotsLock);
        if (!Slots.empty())
            return Fail("the instance base can only change while no console exists");
    }
    if (!SetInstanceBase(base))
        return Fail("instance base must not be negative");
    return 0;
}

int mds_instance_base(void)
{
    return InstanceBase();
}

int mds_mac(int id, uint8_t* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        const auto& mac = inst.NDS->GetFirmware().GetHeader().MacAddr;
        memcpy(out, mac.data(), 6);
        return 0;
    });
}

int mds_save_path(int id, char* out, int max)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    const std::string& path = s->Inst->GetSavePath();
    if (max <= 0 || (int)path.size() >= max)
        return Fail("buffer too small");
    memcpy(out, path.c_str(), path.size() + 1);
    return (int)path.size();
}

}

namespace Headless::Api
{

Instance* FindInstance(int id)
{
    std::lock_guard lock(SlotsLock);
    auto it = Slots.find(id);
    return it == Slots.end() ? nullptr : it->second.Inst.get();
}

std::vector<int> ConsoleIds()
{
    std::lock_guard lock(SlotsLock);
    std::vector<int> ids;
    for (auto& [id, slot] : Slots)
        if (slot.Inst)
            ids.push_back(id);
    return ids;
}

void SetConsoleHooks(std::function<void(Instance&)> created, std::function<void(int)> destroyed)
{
    std::lock_guard lock(HookLock);
    CreatedHook = std::move(created);
    DestroyedHook = std::move(destroyed);
}

void ReadGuest(Instance& inst, int cpu, uint32_t addr, uint32_t len, uint8_t* out)
{
    melonDS::NDS& nds = *inst.NDS;
    int n = cpu == 7 || cpu == 1;
    ARMv5& arm9 = *nds.ARM9;
    for (u32 i = 0; i < len; i++)
    {
        u32 a = addr + i;
        bool tcm = !n && (a < arm9.ITCMSize || (a & arm9.DTCMMask) == arm9.DTCMBase);
        if ((a >> 24) == 0x02 && !tcm)
            out[i] = nds.MainRAM[a & nds.MainRAMMask];
        else
            out[i] = nds.Debugger.Peek(n, a, 1);
    }
}

}
