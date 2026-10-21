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

// The C API of melonds_probes.h: function probes, the memory access log, breakpoint actions and overlay tags
// (src/debug/Probes.*). Reference: docs/headless-api.md, section 5b.

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>

#include "melonds_api.h"
#include "ApiInternal.h"
#include "Instance.h"

using namespace melonDS;
using namespace Headless;

namespace
{

// Features and functions of API level 7 for mds_has (docs/headless-api.md, section 1).
const char* const ProbeFeatures[] = {
    "probes",           // mds_probe_*: function calls with arguments, memory, depth and return values, no parking
    "memlog",           // mds_memlog_*: every CPU/DMA read and write in ranges, no parking
    "bp_actions",       // mds_action_*: return early, set a register, write memory, skip, at an address; logged
    "overlay_tags",     // mds_overlay_tags, mds_overlay_at, the tagged call counts and coverage, probe overlays
    "mds_probe_add", "mds_probe_remove", "mds_probe_list", "mds_probe_capacity", "mds_probe_take",
    "mds_probe_stats", "mds_memlog_add", "mds_memlog_remove", "mds_memlog_list", "mds_memlog_capacity",
    "mds_memlog_take", "mds_memlog_stats", "mds_action_add", "mds_action_remove", "mds_action_list",
    "mds_action_take", "mds_action_stats", "mds_overlay_tags", "mds_overlay_at", "mds_callcounts_take_ov",
    "mds_coverage_take_ov",
};

int Fail(const std::string& error)
{
    Api::SetError(error);
    return -1;
}

// A stopped console with the debugger, as the other debugger calls need.
Instance* Ready(int id)
{
    Instance* inst = Api::FindInstance(id);
    if (!inst)
        Fail("no console " + std::to_string(id));
    else if (inst->GetState() == RunState::Running)
        Fail("console " + std::to_string(id) + " is running");
    else if (!inst->DebugEnabled())
        Fail("console " + std::to_string(id) + " was created without the debugger (debug = 0)");
    else
        return inst;
    return nullptr;
}

bool CountOk(int n, const char* name)
{
    if (n >= 0)
        return true;
    Fail(std::string(name) + " must not be negative");
    return false;
}

template <typename F>
int OnEmu(Instance* inst, F f)
{
    int result = -1;
    std::string error;
    if (!inst->Call([&](Instance& i) { result = f(i.NDS->Debugger.Probes, error); }))
        return Fail("console is shutting down");
    if (result < 0 && !error.empty())
        Fail(error);
    return result;
}

// What a take drained but could not return yet (a call stopped at max), per console. Keyed by id and checked
// against the Instance, so a new console that reuses an id starts clean.
struct Staging
{
    Instance* Inst = nullptr;
    std::vector<ProbeRecord> Probes;
    std::vector<MemLogRecord> Mem;
    std::vector<ActionRecord> Actions;
    std::vector<std::pair<std::pair<u32, int>, u64>> Calls[2];
    std::vector<std::pair<u32, int>> Cover[2];
};

std::mutex StagingLock;
std::map<int, Staging>& Stages = *new std::map<int, Staging>();

Staging& Stage(int id, Instance* inst)
{
    Staging& s = Stages[id];
    if (s.Inst != inst)
        s = Staging {inst};
    return s;
}

// Copy up to max items out of staging (refilled from the console when empty) through put(item, index); put
// returns false to stop before an item (it does not fit). Returns how many were copied.
template <typename T, typename Refill, typename Put>
int TakeStaged(std::vector<T>& staging, Refill refill, int max, Put put)
{
    if (staging.empty())
        staging = refill();
    int n = 0;
    while (n < max && n < (int)staging.size() && put(staging[n], n))
        n++;
    staging.erase(staging.begin(), staging.begin() + n);
    return n;
}

int Stats(int id, uint64_t* out4, std::function<void(DebugProbes&, uint64_t*)> fill)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    if (!out4)
        return Fail("out4 must not be NULL");
    return OnEmu(inst, [&](DebugProbes& p, std::string&) {
        fill(p, out4);
        return 0;
    });
}

uint32_t CpuMask(uint32_t m)
{
    return m & 3;
}

}

namespace Headless::Api
{

bool ProbeFeature(const std::string& name)
{
    for (const char* f : ProbeFeatures)
        if (name == f)
            return true;
    return false;
}

}

extern "C" {

// ---- probes ----------------------------------------------------------------------------------------------------

int mds_probe_add(int id, const mds_probe_spec* spec)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    if (!spec)
        return Fail("spec must not be NULL");
    Probe p {};
    p.CPUMask = CpuMask(spec->cpumask);
    p.Addr = spec->addr;
    p.LRStart = spec->lr_start & ~1u;
    p.LREnd = spec->lr_end;
    p.Flags = spec->flags & MDS_PROBE_NO_RETURN;
    for (int i = 0; i < 4; i++)
    {
        const mds_probe_read& r = spec->reads[i];
        if (r.reg < -1 || r.reg > 15)
            return Fail("probe read " + std::to_string(i) + ": register must be 0-15 or -1");
        if (r.len > MDS_PROBE_MAX_READ)
            return Fail("probe read " + std::to_string(i) + ": at most " + std::to_string(MDS_PROBE_MAX_READ) + " bytes");
        if (r.reg >= 0 && r.len && !(r.when & 3))
            return Fail("probe read " + std::to_string(i) + ": when must be MDS_PROBE_READ_ENTRY and/or _RETURN");
        p.Reads[i] = {(s8)(r.len ? r.reg : -1), r.offset, (u16)r.len, (u8)(r.when & 3)};
    }
    if (!p.CPUMask)
        return Fail("cpumask selects no CPU");
    return OnEmu(inst, [&](DebugProbes& d, std::string& error) {
        int r = d.AddProbe(p);
        if (r < 0)
            error = "bad probe";
        return r;
    });
}

int mds_probe_remove(int id, int probe)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string& error) {
        if (d.RemoveProbe(probe))
            return 0;
        error = "no probe " + std::to_string(probe);
        return -1;
    });
}

int mds_probe_list(int id, int* probes, mds_probe_spec* specs, uint64_t* hits, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string&) {
        const auto& all = d.GetProbes();
        for (int i = 0; i < (int)all.size() && i < max; i++)
        {
            const Probe& p = all[i];
            if (probes)
                probes[i] = p.ID;
            if (hits)
                hits[i] = p.Hits;
            if (specs)
            {
                mds_probe_spec& s = specs[i];
                s = {p.CPUMask, p.Addr, p.LRStart, p.LREnd, p.Flags, {}};
                for (int r = 0; r < 4; r++)
                    s.reads[r] = {p.Reads[r].Reg, p.Reads[r].Offset, p.Reads[r].Len, p.Reads[r].When};
            }
        }
        return (int)all.size();
    });
}

int mds_probe_capacity(int id, int capacity)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(capacity, "capacity"))
        return -1;
    {
        std::lock_guard lock(StagingLock);
        Stage(id, inst).Probes.clear();
    }
    return OnEmu(inst, [&](DebugProbes& d, std::string&) {
        d.SetProbeCapacity(capacity);
        return 0;
    });
}

int mds_probe_take(int id, mds_probe_record* out, uint8_t* data, int max, int max_data)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max") || !CountOk(max_data, "max_data"))
        return -1;
    std::lock_guard lock(StagingLock);
    Staging& s = Stage(id, inst);
    int used = 0;
    return TakeStaged(s.Probes, [inst] {
        std::vector<ProbeRecord> drained;
        inst->Call([&](Instance& i) { drained = i.NDS->Debugger.Probes.DrainProbes(); });
        return drained;
    }, max, [&](const ProbeRecord& r, int n) {
        u32 len = r.Data.size();
        if (used + len > (u32)max_data && n > 0)
            return false;
        u32 copy = std::min<u32>(len, max_data - used);
        if (copy && data)
            memcpy(data + used, r.Data.data(), copy);
        used += copy;
        mds_probe_record& o = out[n];
        o = {};
        o.seq = r.Seq;
        o.frame = r.Frame;
        o.cycle = r.Cycle;
        o.ret_frame = r.RetFrame;
        o.ret_cycle = r.RetCycle;
        o.probe = r.ProbeID;
        o.cpu = r.CPU ? 7 : 9;
        o.kind = r.Kind;
        o.overlay = r.Overlay;
        o.depth = r.Depth;
        o.addr = r.Addr;
        o.lr = r.LR;
        o.sp = r.SP;
        memcpy(o.args, r.Args.data(), sizeof(o.args));
        o.returned = r.Returned;
        o.ret[0] = r.Ret[0];
        o.ret[1] = r.Ret[1];
        o.flags = r.Flags;
        for (int i = 0; i < 4; i++)
        {
            o.entry_len[i] = r.EntryLen[i];
            o.ret_len[i] = r.RetLen[i];
        }
        o.data_len = copy;
        return true;
    });
}

int mds_probe_stats(int id, uint64_t* out4)
{
    return Stats(id, out4, [](DebugProbes& d, uint64_t* o) {
        o[0] = d.ProbeCounts().Recorded;
        o[1] = d.ProbeCounts().Dropped;
        o[2] = d.PendingReturns();
        o[3] = d.ProbesInRing();
    });
}

// ---- memory access log -----------------------------------------------------------------------------------------

int mds_memlog_add(int id, const mds_memlog_spec* spec)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    if (!spec)
        return Fail("spec must not be NULL");
    if (!spec->size)
        return Fail("memlog: size must be > 0");
    if (!(spec->flags & (MDS_MEMLOG_READ | MDS_MEMLOG_WRITE)))
        return Fail("memlog: flags must have MDS_MEMLOG_READ and/or MDS_MEMLOG_WRITE");
    if (!CpuMask(spec->cpumask))
        return Fail("cpumask selects no CPU");
    MemLogRange r {};
    r.CPUMask = CpuMask(spec->cpumask);
    r.Flags = spec->flags & 7;
    r.Start = spec->start;
    u64 end = (u64)spec->start + spec->size - 1;
    r.End = end > 0xFFFFFFFF ? 0xFFFFFFFF : (u32)end;
    r.PCStart = spec->pc_start;
    r.PCEnd = spec->pc_end;
    return OnEmu(inst, [&](DebugProbes& d, std::string& error) {
        int x = d.AddMemLog(r);
        if (x < 0)
            error = "bad memlog range";
        return x;
    });
}

int mds_memlog_remove(int id, int range)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string& error) {
        if (d.RemoveMemLog(range))
            return 0;
        error = "no memlog range " + std::to_string(range);
        return -1;
    });
}

int mds_memlog_list(int id, int* ranges, mds_memlog_spec* specs, uint64_t* hits, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string&) {
        const auto& all = d.GetMemLogs();
        for (int i = 0; i < (int)all.size() && i < max; i++)
        {
            const MemLogRange& r = all[i];
            if (ranges)
                ranges[i] = r.ID;
            if (hits)
                hits[i] = r.Hits;
            if (specs)
                specs[i] = {r.CPUMask, r.Start, r.End - r.Start + 1, r.Flags, r.PCStart, r.PCEnd};
        }
        return (int)all.size();
    });
}

int mds_memlog_capacity(int id, int capacity)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(capacity, "capacity"))
        return -1;
    {
        std::lock_guard lock(StagingLock);
        Stage(id, inst).Mem.clear();
    }
    return OnEmu(inst, [&](DebugProbes& d, std::string&) {
        d.SetMemLogCapacity(capacity);
        return 0;
    });
}

int mds_memlog_take(int id, mds_memlog_record* out, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    std::lock_guard lock(StagingLock);
    Staging& s = Stage(id, inst);
    return TakeStaged(s.Mem, [inst] {
        std::vector<MemLogRecord> drained;
        inst->Call([&](Instance& i) { drained = i.NDS->Debugger.Probes.DrainMemLog(); });
        return drained;
    }, max, [&](const MemLogRecord& r, int n) {
        out[n] = {r.Seq, r.Frame, r.Cycle, r.PC, r.Addr, r.Value, r.Range, (u8)(r.CPU ? 7 : 9), r.Size, r.Flags,
                  r.DMA, 0};
        return true;
    });
}

int mds_memlog_stats(int id, uint64_t* out4)
{
    return Stats(id, out4, [](DebugProbes& d, uint64_t* o) {
        o[0] = d.MemLogCounts().Recorded;
        o[1] = d.MemLogCounts().Dropped;
        o[2] = 0;
        o[3] = d.MemLogInRing();
    });
}

// ---- breakpoint actions ----------------------------------------------------------------------------------------

int mds_action_add(int id, const mds_action_spec* spec)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    if (!spec)
        return Fail("spec must not be NULL");
    if (!CpuMask(spec->cpumask))
        return Fail("cpumask selects no CPU");
    Action a {};
    a.CPUMask = CpuMask(spec->cpumask);
    a.Addr = spec->addr;
    a.Kind = (ActionKind)spec->kind;
    a.Reg = (s8)std::clamp(spec->reg, -1, 16);
    a.Value = spec->value;
    a.Value2 = spec->value2;
    a.Target = spec->target;
    a.Size = spec->size;
    a.LR = spec->lr;
    a.First = spec->first;
    a.Times = spec->times;
    a.Flags = spec->flags & MDS_ACTION_SET_R1;
    switch (spec->kind)
    {
    case MDS_ACTION_RETURN:
        break;
    case MDS_ACTION_SET_REG:
        if (spec->reg < 0 || spec->reg > 15)
            return Fail("action: register must be 0-15");
        break;
    case MDS_ACTION_WRITE:
        if (spec->size != 1 && spec->size != 2 && spec->size != 4)
            return Fail("action: write size must be 1, 2 or 4");
        if ((spec->target >> 24) == 0x04)
            return Fail("action: I/O cannot be written (side effects)");
        break;
    case MDS_ACTION_SKIP:
        if (!spec->size)
            return Fail("action: skip at least 1 instruction");
        break;
    default:
        return Fail("action: unknown kind " + std::to_string(spec->kind));
    }
    return OnEmu(inst, [&](DebugProbes& d, std::string& error) {
        int x = d.AddAction(a);
        if (x < 0)
            error = "bad action";
        return x;
    });
}

int mds_action_remove(int id, int action)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string& error) {
        if (d.RemoveAction(action))
            return 0;
        error = "no action " + std::to_string(action);
        return -1;
    });
}

int mds_action_list(int id, int* actions, mds_action_spec* specs, uint64_t* matched, uint64_t* fired, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string&) {
        const auto& all = d.GetActions();
        for (int i = 0; i < (int)all.size() && i < max; i++)
        {
            const Action& a = all[i];
            if (actions)
                actions[i] = a.ID;
            if (matched)
                matched[i] = a.Matched;
            if (fired)
                fired[i] = a.Fired;
            if (specs)
                specs[i] = {a.CPUMask, a.Addr, (int32_t)a.Kind, a.Reg, a.Value, a.Value2, a.Target, a.Size, a.LR,
                            a.First, a.Times, a.Flags};
        }
        return (int)all.size();
    });
}

int mds_action_take(int id, mds_action_record* out, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    std::lock_guard lock(StagingLock);
    Staging& s = Stage(id, inst);
    return TakeStaged(s.Actions, [inst] {
        std::vector<ActionRecord> drained;
        inst->Call([&](Instance& i) { drained = i.NDS->Debugger.Probes.DrainActions(); });
        return drained;
    }, max, [&](const ActionRecord& r, int n) {
        mds_action_record& o = out[n];
        o = {};
        o.seq = r.Seq;
        o.frame = r.Frame;
        o.cycle = r.Cycle;
        o.hit = r.Hit;
        o.action = r.ActionID;
        o.cpu = r.CPU ? 7 : 9;
        o.kind = (int32_t)r.Kind;
        o.pc = r.PC;
        o.lr = r.LR;
        memcpy(o.regs, r.Regs.data(), sizeof(o.regs));
        o.value = r.Value;
        o.target = r.Target;
        return true;
    });
}

int mds_action_stats(int id, uint64_t* out4)
{
    return Stats(id, out4, [](DebugProbes& d, uint64_t* o) {
        o[0] = d.ActionCounts().Recorded;
        o[1] = d.ActionCounts().Dropped;
        o[2] = 0;
        o[3] = d.ActionsInRing();
    });
}

// ---- overlay tags ----------------------------------------------------------------------------------------------

int mds_overlay_tags(int id, int enabled)
{
    Instance* inst = Ready(id);
    if (!inst)
        return -1;
    return OnEmu(inst, [&](DebugProbes& d, std::string&) {
        d.SetTagging(enabled != 0);
        return 0;
    });
}

int mds_overlay_at(int id, uint32_t addr)
{
    Instance* inst = Ready(id);
    if (!inst)
        return INT32_MIN;
    int result = INT32_MIN;
    if (!inst->Call([&](Instance& i) { result = i.NDS->Debugger.Probes.OverlayAt(addr); }))
    {
        Fail("console is shutting down");
        return INT32_MIN;
    }
    return result;
}

int mds_callcounts_take_ov(int id, int cpu, uint32_t* entries, int32_t* overlays, uint64_t* counts, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    int n = cpu == 7 || cpu == 1;
    std::lock_guard lock(StagingLock);
    Staging& s = Stage(id, inst);
    return TakeStaged(s.Calls[n], [inst, n] {
        std::vector<std::pair<std::pair<u32, int>, u64>> drained;
        inst->Call([&](Instance& i) { drained = i.NDS->Debugger.Probes.DrainCallCountsTagged(n); });
        return drained;
    }, max, [&](const std::pair<std::pair<u32, int>, u64>& r, int k) {
        if (entries)
            entries[k] = r.first.first;
        if (overlays)
            overlays[k] = r.first.second;
        if (counts)
            counts[k] = r.second;
        return true;
    });
}

int mds_coverage_take_ov(int id, int cpu, uint32_t* addrs, int32_t* overlays, int max)
{
    Instance* inst = Ready(id);
    if (!inst || !CountOk(max, "max"))
        return -1;
    int n = cpu == 7 || cpu == 1;
    std::lock_guard lock(StagingLock);
    Staging& s = Stage(id, inst);
    return TakeStaged(s.Cover[n], [inst, n] {
        std::vector<std::pair<u32, int>> drained;
        inst->Call([&](Instance& i) { drained = i.NDS->Debugger.Probes.DrainCoverageTagged(n); });
        return drained;
    }, max, [&](const std::pair<u32, int>& r, int k) {
        if (addrs)
            addrs[k] = r.first;
        if (overlays)
            overlays[k] = r.second;
        return true;
    });
}

}

static_assert(sizeof(mds_probe_record) == 128, "mds_probe_record layout");
static_assert(sizeof(mds_memlog_record) == 48, "mds_memlog_record layout");
static_assert(sizeof(mds_action_record) == 80, "mds_action_record layout");
static_assert(sizeof(mds_probe_spec) == 84, "mds_probe_spec layout");
static_assert(sizeof(mds_action_spec) == 48, "mds_action_spec layout");
