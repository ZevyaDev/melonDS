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

#include "Debugger.h"
#include "../ARM.h"
#include "../NDS.h"

namespace melonDS
{

constexpr u32 PageShift = 12;
constexpr u32 PageCount = 1u << (32 - PageShift);
constexpr size_t MaxCallDepth = 4096;
constexpr u32 StackSwitchDistance = 0x400;
constexpr u32 StackMatchWindow = 0x200;
constexpr size_t MaxParkedStacks = 64;
constexpr u32 MainRAMWords = 0x400000 >> 2;

Debugger::Debugger(melonDS::NDS& nds) noexcept : NDS(nds)
{
}

void Debugger::Reset()
{
    for (int i = 0; i < 2; i++)
    {
        BreakRequested[i] = false;
        StepRemaining[i] = 0;
        PendingValid[i] = false;
        CallStack[i].clear();
        ParkedStacks[i].clear();
        Exceptions[i].clear();
        LastSP[i] = {};
        CurPC[i] = 0;
    }
    TraceHead = 0;
    TraceWrapped = false;
    if (!LastWriter.empty())
        std::fill(LastWriter.begin(), LastWriter.end(), 0);
    Rearm();
}

int Debugger::AddBreakpoint(u8 cpuMask, u32 addr, BreakCallback callback)
{
    Breakpoints.push_back({NextID, cpuMask, true, addr & ~1u, 0, std::move(callback)});
    RebuildPages();
    return NextID++;
}

int Debugger::AddWatchpoint(u8 cpuMask, u32 start, u32 size, u8 flags, BreakCallback callback)
{
    if (size == 0 || !(flags & (Watch_Read | Watch_Write)))
        return -1;
    u32 end = start + size - 1;
    if (end < start)
        end = 0xFFFFFFFF;
    Watchpoints.push_back({NextID, cpuMask, flags, true, start, end, 0, std::move(callback)});
    RebuildPages();
    return NextID++;
}

bool Debugger::Remove(int id)
{
    auto bp = std::find_if(Breakpoints.begin(), Breakpoints.end(), [id](const Breakpoint& b) { return b.ID == id; });
    if (bp != Breakpoints.end())
    {
        Breakpoints.erase(bp);
        RebuildPages();
        return true;
    }
    auto wp = std::find_if(Watchpoints.begin(), Watchpoints.end(), [id](const Watchpoint& w) { return w.ID == id; });
    if (wp != Watchpoints.end())
    {
        Watchpoints.erase(wp);
        RebuildPages();
        return true;
    }
    return false;
}

bool Debugger::SetEnabled(int id, bool enabled)
{
    for (auto& b : Breakpoints)
        if (b.ID == id) { b.Enabled = enabled; RebuildPages(); return true; }
    for (auto& w : Watchpoints)
        if (w.ID == id) { w.Enabled = enabled; RebuildPages(); return true; }
    return false;
}

void Debugger::ClearAll()
{
    Breakpoints.clear();
    Watchpoints.clear();
    RebuildPages();
}

void Debugger::SetBreakHandler(std::function<void(const BreakEvent&)> handler)
{
    Handler = std::move(handler);
}

void Debugger::SetServiceHandler(std::function<void()> handler)
{
    ServiceHandler = std::move(handler);
}

void Debugger::RequestService()
{
    ServiceRequested = true;
    Armed[0] = true;
    Armed[1] = true;
}

void Debugger::RequestBreak(u8 cpuMask)
{
    for (int i = 0; i < 2; i++)
        if (cpuMask & (1 << i))
        {
            BreakRequested[i] = true;
            Armed[i] = true;
        }
}

void Debugger::CancelBreakRequests()
{
    BreakRequested[0] = false;
    BreakRequested[1] = false;
    StepRemaining = {};
    Rearm();
}

void Debugger::Step(u8 cpu, u32 count)
{
    u8 n = cpu & 1;
    // Parked inside this CPU's instruction hook, the instruction at the break has passed its hook already and
    // runs on resume, so it is the first of the count. Otherwise the next hook call is for the first one, and
    // the countdown reaching zero there would park before anything ran.
    if (count && InstrBreakCPU != n && count != 0xFFFFFFFF)
        count++;
    StepRemaining[n] = count;
    Rearm();
}

void Debugger::OnStateLoaded()
{
    for (int i = 0; i < 2; i++)
    {
        CallStack[i].clear();
        ParkedStacks[i].clear();
        Exceptions[i].clear();
        LastSP[i] = {};
        LastMode[i] = 0;
        PrevInstr[i] = 0;
        PrevThumb[i] = false;
        PendingValid[i] = false;
    }
    ARM* cpus[2] = {NDS.ARM9, NDS.ARM7};
    for (int i = 0; i < 2; i++)
        CurPC[i] = cpus[i]->R[15] - ((cpus[i]->CPSR & 0x20) ? 2 : 4);
    if (!LastWriter.empty())
        std::fill(LastWriter.begin(), LastWriter.end(), 0);
    CoverageLastBits = {};
    Rearm();
}

void Debugger::SetTrace(u8 cpuMask, u32 start, u32 end, u32 capacity)
{
    TraceMask = capacity ? cpuMask : 0;
    TraceStart = start;
    TraceEnd = end;
    Trace.assign(TraceMask ? capacity : 0, {});
    TraceHead = 0;
    TraceWrapped = false;
    Rearm();
}

std::vector<TraceEntry> Debugger::DrainTrace()
{
    std::vector<TraceEntry> out;
    if (TraceWrapped)
    {
        out.insert(out.end(), Trace.begin() + TraceHead, Trace.end());
        out.insert(out.end(), Trace.begin(), Trace.begin() + TraceHead);
    }
    else
        out.insert(out.end(), Trace.begin(), Trace.begin() + TraceHead);
    TraceHead = 0;
    TraceWrapped = false;
    return out;
}

void Debugger::SetCallTracking(bool enabled)
{
    CallTracking = enabled;
    if (!enabled)
        for (int i = 0; i < 2; i++)
        {
            CallStack[i].clear();
            ParkedStacks[i].clear();
            Exceptions[i].clear();
            LastSP[i] = {};
        }
    Rearm();
}

std::vector<CallFrame> Debugger::GetCallStack(u8 cpu) const
{
    std::vector<CallFrame> out;
    for (auto& ex : Exceptions[cpu & 1])
        out.insert(out.end(), ex.Stack.begin(), ex.Stack.end());
    out.insert(out.end(), CallStack[cpu & 1].begin(), CallStack[cpu & 1].end());
    return out;
}

std::unordered_map<u32, u64> Debugger::DrainCallCounts(u8 cpu)
{
    std::unordered_map<u32, u64> out;
    out.swap(CallCounts[cpu & 1]);
    return out;
}

void Debugger::SetCoverage(bool enabled)
{
    Coverage = enabled;
    Rearm();
}

std::vector<u32> Debugger::DrainCoverage(u8 cpu)
{
    std::vector<u32> out;
    for (auto& [page, bits] : CoveragePages[cpu & 1])
        for (u32 w = 0; w < bits.size(); w++)
            for (u64 b = bits[w]; b; b &= b - 1)
                out.push_back((page << PageShift) | (((w << 6) | __builtin_ctzll(b)) << 1));
    std::sort(out.begin(), out.end());
    CoveragePages[cpu & 1].clear();
    CoverageLastBits[cpu & 1] = nullptr;
    return out;
}

void Debugger::SetLastWriter(bool enabled)
{
    if (enabled)
        LastWriter.assign(MainRAMWords, 0);
    else
        LastWriter.clear();
    Rearm();
}

u32 Debugger::GetLastWriter(u32 addr) const
{
    if (LastWriter.empty() || (addr >> 24) != 0x02)
        return 0;
    return LastWriter[(addr & 0x3FFFFF) >> 2];
}

u32 Debugger::Peek(u8 cpu, u32 addr, u8 size) const
{
    addr &= ~(u32)(size - 1);
    if (cpu == 0)
    {
        ARMv5& arm9 = *NDS.ARM9;
        const u8* tcm = nullptr;
        if (addr < arm9.ITCMSize)
            tcm = &arm9.ITCM[addr & (ITCMPhysicalSize - 1)];
        else if ((addr & arm9.DTCMMask) == arm9.DTCMBase)
            tcm = &arm9.DTCM[addr & (DTCMPhysicalSize - 1)];
        if (tcm)
        {
            if (size == 1) return *tcm;
            if (size == 2) return *(const u16*)tcm;
            return *(const u32*)tcm;
        }
    }
    if ((addr >> 24) == 0x04)
        return 0;
    if (cpu == 0)
    {
        if (size == 1) return NDS.ARM9Read8(addr);
        if (size == 2) return NDS.ARM9Read16(addr);
        return NDS.ARM9Read32(addr);
    }
    if (size == 1) return NDS.ARM7Read8(addr);
    if (size == 2) return NDS.ARM7Read16(addr);
    return NDS.ARM7Read32(addr);
}

bool Debugger::Poke(u8 cpu, u32 addr, u8 size, u32 value)
{
    addr &= ~(u32)(size - 1);
    if (cpu == 0)
    {
        ARMv5& arm9 = *NDS.ARM9;
        u8* tcm = nullptr;
        if (addr < arm9.ITCMSize)
            tcm = &arm9.ITCM[addr & (ITCMPhysicalSize - 1)];
        else if ((addr & arm9.DTCMMask) == arm9.DTCMBase)
            tcm = &arm9.DTCM[addr & (DTCMPhysicalSize - 1)];
        if (tcm)
        {
            if (size == 1) *tcm = value;
            else if (size == 2) *(u16*)tcm = value;
            else *(u32*)tcm = value;
            return true;
        }
    }
    if ((addr >> 24) == 0x04)
        return false;
    if (cpu == 0)
    {
        if (size == 1) NDS.ARM9Write8(addr, value);
        else if (size == 2) NDS.ARM9Write16(addr, value);
        else NDS.ARM9Write32(addr, value);
    }
    else
    {
        if (size == 1) NDS.ARM7Write8(addr, value);
        else if (size == 2) NDS.ARM7Write16(addr, value);
        else NDS.ARM7Write32(addr, value);
    }
    return true;
}

void Debugger::Rearm()
{
    CallsArmed = CallTracking;
    for (int i = 0; i < 2; i++)
    {
        bool exec = false;
        bool data = !LastWriter.empty();
        for (auto& b : Breakpoints)
            exec |= b.Enabled && (b.CPUMask & (1 << i));
        for (auto& w : Watchpoints)
            data |= w.Enabled && (w.CPUMask & (1 << i));
        DataArmed[i] = data;
        Armed[i] = exec || data || CallTracking || Coverage || (TraceMask & (1 << i))
            || StepRemaining[i] || PendingValid[i] || BreakRequested[i] || ServiceRequested;
    }
}

void Debugger::RebuildPages()
{
    for (int i = 0; i < 2; i++)
    {
        ExecPages[i].clear();
        WatchPages[i].clear();
        for (auto& b : Breakpoints)
        {
            if (!b.Enabled || !(b.CPUMask & (1 << i)))
                continue;
            if (ExecPages[i].empty())
                ExecPages[i].assign(PageCount, 0);
            ExecPages[i][b.Addr >> PageShift] = 1;
        }
        for (auto& w : Watchpoints)
        {
            if (!w.Enabled || !(w.CPUMask & (1 << i)))
                continue;
            if (WatchPages[i].empty())
                WatchPages[i].assign(PageCount, 0);
            for (u64 p = w.Start >> PageShift; p <= (w.End >> PageShift); p++)
                WatchPages[i][p] = 1;
        }
    }
    Rearm();
}

void Debugger::Raise(const BreakEvent& ev)
{
    if (!Handler || InBreak)
        return;
    InBreak = true;
    Handler(ev);
    InBreak = false;
    Rearm();
}

void Debugger::SwitchStacks(u8 n, u32 oldSP, u32 newSP)
{
    auto& parked = ParkedStacks[n];
    if (!CallStack[n].empty())
    {
        if (parked.size() >= MaxParkedStacks)
            parked.erase(parked.begin());
        parked[oldSP] = std::move(CallStack[n]);
    }
    CallStack[n].clear();
    auto best = parked.end();
    u32 bestDist = StackMatchWindow + 1;
    for (auto it = parked.lower_bound(newSP > StackMatchWindow ? newSP - StackMatchWindow : 0);
         it != parked.end() && it->first <= newSP + StackMatchWindow; ++it)
    {
        u32 dist = it->first > newSP ? it->first - newSP : newSP - it->first;
        if (dist < bestDist)
        {
            best = it;
            bestDist = dist;
        }
    }
    if (best != parked.end())
    {
        CallStack[n] = std::move(best->second);
        parked.erase(best);
    }
}

static bool IsExceptionMode(u8 mode)
{
    return mode != 0x10 && mode != 0x1F;
}

static bool IsExceptionReturn(u32 instr, bool thumb)
{
    if (thumb)
        return false;
    if ((instr & 0x0C000000) == 0)
        return (instr & (1 << 20)) && ((instr >> 12) & 0xF) == 15;
    if ((instr & 0x0E000000) == 0x08000000)
        return (instr & (1 << 20)) && (instr & (1 << 22)) && (instr & (1 << 15));
    return false;
}

static bool IsStackAdjust(u32 instr, bool thumb)
{
    if (thumb)
    {
        u16 t = instr & 0xFFFF;
        return (t & 0xFF00) == 0xB000 || (t & 0xF600) == 0xB400;
    }
    u32 rn = (instr >> 16) & 0xF;
    u32 rd = (instr >> 12) & 0xF;
    if ((instr & 0x0C000000) == 0)
    {
        u32 op = (instr >> 21) & 0xF;
        return rd == 13 && rn == 13 && (op == 0x2 || op == 0x4);
    }
    if ((instr & 0x0E000000) == 0x08000000)
        return rn == 13 && (instr & (1 << 21)) && !(instr & (1 << 13));
    if ((instr & 0x0C000000) == 0x04000000)
        return rn == 13 && rd != 13 && (!(instr & (1 << 24)) || (instr & (1 << 21)));
    return false;
}

void Debugger::UnwindCalls(ARM& cpu, u32 pc)
{
    u8 n = cpu.Num;
    auto& stack = CallStack[n];
    u8 mode = cpu.CPSR & 0x1F;
    u32 sp = cpu.R[13];

    if (!Exceptions[n].empty() && IsExceptionMode(LastMode[n]) && IsExceptionReturn(PrevInstr[n], PrevThumb[n]))
    {
        Interrupted ex = std::move(Exceptions[n].back());
        Exceptions[n].pop_back();
        if ((ex.Return & ~1u) == pc || IsExceptionMode(mode))
            stack = std::move(ex.Stack);
        else
        {
            CallStack[n] = std::move(ex.Stack);
            SwitchStacks(n, ex.SP, sp);
        }
        LastSP[n][(mode == 0x1F ? 0x10 : mode) & 0xF] = sp;
        LastMode[n] = mode;
    }

    u32& last = LastSP[n][(mode == 0x1F ? 0x10 : mode) & 0xF];
    if (last && sp != last && Exceptions[n].empty())
    {
        bool sameBank = mode == LastMode[n];
        u32 delta = sp > last ? sp - last : last - sp;
        if ((sameBank && !IsStackAdjust(PrevInstr[n], PrevThumb[n])) || delta >= StackSwitchDistance)
            SwitchStacks(n, last, sp);
    }
    last = sp;
    LastMode[n] = mode;

    size_t depth = stack.size();
    for (size_t k = 0; k < std::min<size_t>(depth, 8); k++)
    {
        const CallFrame& f = stack[depth - 1 - k];
        if ((f.Return & ~1u) == pc && f.Mode == mode && cpu.R[13] >= f.SP)
        {
            stack.resize(depth - 1 - k);
            return;
        }
    }
    while (!stack.empty())
    {
        CallFrame& top = stack.back();
        if (top.Mode != mode || cpu.R[13] <= top.SP)
            return;
        stack.pop_back();
    }
}

void Debugger::OnInstr(ARM& cpu, u32 pc)
{
    u8 n = cpu.Num;
    CurPC[n] = pc;

    if (ServiceRequested.load(std::memory_order_relaxed) && ServiceRequested.exchange(false))
    {
        if (ServiceHandler)
            ServiceHandler();
        Rearm();
    }

    if (CallTracking)
    {
        UnwindCalls(cpu, pc);
        PrevInstr[n] = cpu.NextInstr[0];
        PrevThumb[n] = cpu.CPSR & 0x20;
    }

    if (Coverage)
    {
        u32 page = pc >> PageShift;
        if (!CoverageLastBits[n] || CoverageLastPage[n] != page)
        {
            CoverageLastBits[n] = &CoveragePages[n][page];
            CoverageLastPage[n] = page;
        }
        u32 idx = (pc & ((1u << PageShift) - 1)) >> 1;
        (*CoverageLastBits[n])[idx >> 6] |= 1ull << (idx & 63);
    }

    if ((TraceMask & (1 << n)) && pc >= TraceStart && pc <= TraceEnd)
    {
        u64 ts = n ? NDS.ARM7Timestamp : (NDS.ARM9Timestamp >> NDS.ARM9ClockShift);
        Trace[TraceHead] = {ts, pc, cpu.NextInstr[0], cpu.CPSR, n};
        if (++TraceHead == Trace.size())
        {
            TraceHead = 0;
            TraceWrapped = true;
        }
    }

    bool raise = false;
    BreakEvent ev {BreakKind::Step, n, 0, 0, pc, pc, 0};

    if (PendingValid[n])
    {
        PendingValid[n] = false;
        ev = Pending[n];
        raise = true;
    }
    if (BreakRequested[n].exchange(false) && !raise)
    {
        ev.Kind = BreakKind::Request;
        raise = true;
    }
    if (StepRemaining[n] && --StepRemaining[n] == 0 && !raise)
    {
        ev.Kind = BreakKind::Step;
        raise = true;
    }

    if (!ExecPages[n].empty() && ExecPages[n][pc >> PageShift])
    {
        std::vector<int> hits;
        for (auto& b : Breakpoints)
            if (b.Enabled && (b.CPUMask & (1 << n)) && b.Addr == pc)
            {
                b.Hits++;
                hits.push_back(b.ID);
            }
        for (int id : hits)
        {
            auto it = std::find_if(Breakpoints.begin(), Breakpoints.end(), [id](const Breakpoint& b) { return b.ID == id; });
            if (it == Breakpoints.end())
                continue;
            BreakEvent hit {BreakKind::Exec, n, 0, id, pc, pc, 0};
            BreakCallback cb = it->Callback;
            if ((!cb || cb(hit)) && !raise)
            {
                ev = hit;
                raise = true;
            }
        }
    }

    if (raise)
    {
        InstrBreakCPU = n;
        Raise(ev);
        InstrBreakCPU = -1;
    }
}

void Debugger::OnDMA(u8 n, u8 channel, u32 addr, u8 size, u32 value)
{
    if (!LastWriter.empty() && (addr >> 24) == 0x02)
        LastWriter[(addr & 0x3FFFFF) >> 2] = DMAWriterTag | (channel << 1) | n;
    WatchHit(n, addr, size, value, true, channel);
}

void Debugger::OnData(ARM& cpu, u32 addr, u8 size, u32 value, bool write)
{
    u8 n = cpu.Num;
    if (write && !LastWriter.empty() && (addr >> 24) == 0x02)
        LastWriter[(addr & 0x3FFFFF) >> 2] = CurPC[n] | n;
    WatchHit(n, addr, size, value, write, 0xFF);
}

void Debugger::WatchHit(u8 n, u32 addr, u8 size, u32 value, bool write, u8 dma)
{
    if (WatchPages[n].empty() || !WatchPages[n][addr >> PageShift])
        return;

    u8 kind = write ? Watch_Write : Watch_Read;
    u32 last = addr + size - 1;
    std::vector<int> hits;
    for (auto& w : Watchpoints)
        if (w.Enabled && (w.CPUMask & (1 << n)) && (w.Flags & kind) && addr <= w.End && last >= w.Start)
        {
            w.Hits++;
            hits.push_back(w.ID);
        }
    if (hits.empty())
        return;

    if (!write)
        value = Peek(n, addr, size);
    bool raiseDMA = false;
    BreakEvent dmaEvent {};
    for (int id : hits)
    {
        auto it = std::find_if(Watchpoints.begin(), Watchpoints.end(), [id](const Watchpoint& w) { return w.ID == id; });
        if (it == Watchpoints.end())
            continue;
        // A DMA write is not made by an instruction: its pc is the DMA tag, and it parks at once (the DMA
        // is between two units, the CPUs between two instructions) instead of at the CPU's next instruction.
        u32 pc = dma == 0xFF ? CurPC[n] : (DMAWriterTag | (dma << 1) | n);
        BreakEvent hit {write ? BreakKind::Write : BreakKind::Read, n, size, id, pc, addr, value, dma};
        BreakCallback cb = it->Callback;
        if (cb && !cb(hit))
            continue;
        if (dma != 0xFF)
        {
            if (!raiseDMA)
            {
                dmaEvent = hit;
                raiseDMA = true;
            }
        }
        else if (!PendingValid[n])
        {
            Pending[n] = hit;
            PendingValid[n] = true;
            Armed[n] = true;
        }
    }
    if (raiseDMA)
        Raise(dmaEvent);
}

void Debugger::OnCall(ARM& cpu)
{
    u8 n = cpu.Num;
    u32 entry = cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
    auto& stack = CallStack[n];
    if (stack.size() >= MaxCallDepth)
        stack.erase(stack.begin());
    stack.push_back({CurPC[n], entry, cpu.R[14] & ~1u, cpu.R[13], (u8)(cpu.CPSR & 0x1F), false});
    CallCounts[n][entry]++;
}

void Debugger::OnException(ARM& cpu, u32 ret)
{
    u8 n = cpu.Num;
    u32 entry = cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
    u8 from = LastMode[n];
    u32 fromSP = LastSP[n][(from == 0x1F ? 0x10 : from) & 0xF];
    if (Exceptions[n].size() >= 16)
        Exceptions[n].erase(Exceptions[n].begin());
    Exceptions[n].push_back({ret, fromSP, std::move(CallStack[n])});
    CallStack[n] = {{ret, entry, ret, cpu.R[13], (u8)(cpu.CPSR & 0x1F), true}};
}

void Debugger::OnFault(ARM& cpu, u32 pc, u32 addr)
{
    u8 n = cpu.Num;
    Raise({BreakKind::Fault, n, 0, 0, pc, addr, 0});
}

}
