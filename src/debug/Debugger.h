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

#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "../types.h"

namespace melonDS
{
class ARM;
class NDS;

enum class BreakKind : u8
{
    Exec,
    Read,
    Write,
    Step,
    Request,
    Fault,
};

enum WatchFlags : u8
{
    Watch_Read = 1,
    Watch_Write = 2,
};

struct BreakEvent
{
    BreakKind Kind;
    u8 CPU;
    u8 Size;
    int ID;
    u32 PC;
    u32 Addr;
    u32 Value;
    u8 DMAChannel = 0xFF;
};

using BreakCallback = std::function<bool(const BreakEvent&)>;

struct Breakpoint
{
    int ID;
    u8 CPUMask;
    bool Enabled;
    u32 Addr;
    u64 Hits;
    BreakCallback Callback;
};

struct Watchpoint
{
    int ID;
    u8 CPUMask;
    u8 Flags;
    bool Enabled;
    u32 Start;
    u32 End;
    u64 Hits;
    BreakCallback Callback;
};

struct TraceEntry
{
    u64 Timestamp;
    u32 PC;
    u32 Instr;
    u32 CPSR;
    u8 CPU;
};

struct CallFrame
{
    u32 Site;
    u32 Entry;
    u32 Return;
    u32 SP;
    u8 Mode;
    bool Exception;
};

class Debugger
{
public:
    explicit Debugger(melonDS::NDS& nds) noexcept;

    void Reset();

    int AddBreakpoint(u8 cpuMask, u32 addr, BreakCallback callback = nullptr);
    int AddWatchpoint(u8 cpuMask, u32 start, u32 size, u8 flags, BreakCallback callback = nullptr);
    bool Remove(int id);
    bool SetEnabled(int id, bool enabled);
    void ClearAll();
    const std::vector<Breakpoint>& GetBreakpoints() const { return Breakpoints; }
    const std::vector<Watchpoint>& GetWatchpoints() const { return Watchpoints; }

    void SetBreakHandler(std::function<void(const BreakEvent&)> handler);
    void SetServiceHandler(std::function<void()> handler);
    void RequestService();
    void RequestBreak(u8 cpuMask);
    void CancelBreakRequests();
    void Step(u8 cpu, u32 count);

    void SetTrace(u8 cpuMask, u32 start, u32 end, u32 capacity);
    std::vector<TraceEntry> DrainTrace();

    void SetCallTracking(bool enabled);
    std::vector<CallFrame> GetCallStack(u8 cpu) const;
    std::unordered_map<u32, u64> DrainCallCounts(u8 cpu);

    void SetCoverage(bool enabled);
    std::vector<u32> DrainCoverage(u8 cpu);

    void SetLastWriter(bool enabled);
    u32 GetLastWriter(u32 addr) const;

    u32 Peek(u8 cpu, u32 addr, u8 size) const;
    bool Poke(u8 cpu, u32 addr, u8 size, u32 value);

    bool IsBreaking() const { return InBreak; }

    std::array<bool, 2> Armed {};
    std::array<bool, 2> DataArmed {};
    bool CallsArmed = false;

    void OnInstr(ARM& cpu, u32 pc);
    void OnData(ARM& cpu, u32 addr, u8 size, u32 value, bool write);
    void OnDMA(u8 cpu, u8 channel, u32 addr, u8 size, u32 value);
    void OnCall(ARM& cpu);
    void OnException(ARM& cpu, u32 ret);
    void OnFault(ARM& cpu, u32 pc, u32 addr);

    std::array<u32, 2> CurPC {};

private:
    melonDS::NDS& NDS;
    int NextID = 1;
    std::vector<Breakpoint> Breakpoints;
    std::vector<Watchpoint> Watchpoints;
    std::array<std::vector<u8>, 2> ExecPages;
    std::array<std::vector<u8>, 2> WatchPages;
    std::function<void(const BreakEvent&)> Handler;
    std::function<void()> ServiceHandler;
    std::atomic<bool> ServiceRequested {false};
    std::array<std::atomic<bool>, 2> BreakRequested {};
    std::array<u32, 2> StepRemaining {};
    std::array<bool, 2> PendingValid {};
    std::array<BreakEvent, 2> Pending {};
    bool InBreak = false;

    u8 TraceMask = 0;
    u32 TraceStart = 0, TraceEnd = 0;
    std::vector<TraceEntry> Trace;
    size_t TraceHead = 0;
    bool TraceWrapped = false;

    bool CallTracking = false;
    std::array<std::vector<CallFrame>, 2> CallStack;
    std::array<std::map<u32, std::vector<CallFrame>>, 2> ParkedStacks;
    std::array<std::array<u32, 16>, 2> LastSP {};
    std::array<u8, 2> LastMode {};
    std::array<u32, 2> PrevInstr {};
    std::array<bool, 2> PrevThumb {};

    struct Interrupted
    {
        u32 Return;
        u32 SP;
        std::vector<CallFrame> Stack;
    };
    std::array<std::vector<Interrupted>, 2> Exceptions;
    std::array<std::unordered_map<u32, u64>, 2> CallCounts;

    bool Coverage = false;
    std::array<std::unordered_map<u32, std::array<u64, 32>>, 2> CoveragePages;
    std::array<u32, 2> CoverageLastPage {};
    std::array<std::array<u64, 32>*, 2> CoverageLastBits {};

    std::vector<u32> LastWriter;

    void Rearm();
    void RebuildPages();
    void Raise(const BreakEvent& ev);
    void UnwindCalls(ARM& cpu, u32 pc);
    void WatchHit(u8 n, u32 addr, u8 size, u32 value, bool write, u8 dma);
    void SwitchStacks(u8 n, u32 oldSP, u32 newSP);
};

}

#endif
