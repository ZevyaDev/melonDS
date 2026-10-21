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

#ifndef DEBUG_PROBES_H
#define DEBUG_PROBES_H

// Analysis tools that log without parking the console (docs/headless-api.md, section 5b):
//
// - function probes: every call of a function with its arguments, memory behind chosen registers, the caller and
//   the call depth, and the return value once that call returns;
// - the memory access log: every CPU (and DMA) read and/or write in address ranges;
// - breakpoint actions: return early with a value, set a register, write memory or skip instructions at an
//   address, every hit or chosen ones, each one logged;
// - overlay attribution: which ARM9 overlay of the cart the code at an address belongs to now, judged from the
//   memory there, used to tag probe records, call counts and coverage.
//
// Everything here runs on the emulator thread inside the debugger hooks (Debugger::OnInstr, OnData, OnDMA). The
// probes and the log only read memory, without side effects, so a run with them gives the same emulation as one
// without; actions change the run by design and are all logged.

#include <array>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "../types.h"
#include "RomOverlays.h"

namespace melonDS
{
class ARM;
class NDS;
class Debugger;

// Overlay ids of the attribution; >= 0 is a cart overlay id.
constexpr int OverlayStatic = -1;     // no overlay covers the address (main module, TCM, ...), or ARM7 code
constexpr int OverlayAmbiguous = -2;  // several overlays match the memory there equally well
constexpr int OverlayUnknown = -3;    // inside overlays' ranges, but the memory matches none of them

// The overlay whose code is at addr now. candidates: indices into overlays whose code range covers addr. peek32
// reads an aligned ARM9 word without side effects. The candidate with the most equal words in a window from addr
// wins (16 words, then 64); a tie is broken by the words at the overlays' starts; with fewer than half the words
// equal the code there is OverlayUnknown. Pure (tested in debug_common_tests).
int MatchOverlay(const std::vector<RomOverlay>& overlays, const std::vector<int>& candidates, u32 addr,
                 const std::function<u32(u32)>& peek32);

struct ProbeRead
{
    s8 Reg = -1;        // the register holding the pointer, 0-15; -1 unused
    s32 Offset = 0;     // added to the register's value
    u16 Len = 0;        // bytes to read, at most ProbeMaxRead
    u8 When = 0;        // ProbeRead_Entry | ProbeRead_Return
};

constexpr u32 ProbeMaxRead = 256;
constexpr int ProbeReads = 4;
enum : u8 { ProbeRead_Entry = 1, ProbeRead_Return = 2 };
enum : u32 { Probe_NoReturn = 1 };              // Probe::Flags: do not wait for the call's return
enum : u32 { ProbeRec_Forced = 1, ProbeRec_ReturnLost = 2 };    // ProbeRecord::Flags

struct Probe
{
    int ID;
    u8 CPUMask;
    u32 Addr;
    u32 LRStart, LREnd;     // record only calls whose return address is in [LRStart, LREnd]; 0, 0 = every call
    u32 Flags;
    std::array<ProbeRead, ProbeReads> Reads;
    u64 Hits;
};

struct ProbeRecord
{
    u64 Seq;            // call number (kind 0); for a late return, the call it belongs to
    u64 Frame, Cycle;
    u64 RetFrame, RetCycle;
    int ProbeID;
    u8 CPU;             // 0 ARM9, 1 ARM7
    u8 Kind;            // 0 a call (with its return if it came before the record was taken), 1 a late return
    bool Returned;
    int Overlay;
    s32 Depth;          // call tracker depth at entry, -1 with call tracking off
    u32 Addr, LR, SP;
    std::array<u32, 4> Args;
    std::array<u32, 2> Ret;
    u32 Flags;
    std::array<u16, ProbeReads> EntryLen, RetLen;
    std::vector<u8> Data;   // the entry reads in order, then the return reads
};

enum : u8 { MemLog_Read = 1, MemLog_Write = 2, MemLog_DMA = 4 };

struct MemLogRange
{
    int ID;
    u8 CPUMask;
    u8 Flags;           // MemLog_Read | MemLog_Write, and MemLog_DMA to log DMA units too
    u32 Start, End;     // inclusive
    u32 PCStart, PCEnd; // only accesses by instructions in [PCStart, PCEnd]; 0, 0 = any (DMA units always pass)
    u64 Hits;
};

struct MemLogRecord
{
    u64 Seq, Frame, Cycle;
    u32 PC;             // the accessing instruction; for a DMA unit the DMA tag 0xFFFFFFF0 | channel << 1 | arm7
    u32 Addr, Value;
    u8 CPU, Size;
    u8 Flags;           // MemLog_Read or MemLog_Write, | MemLog_DMA
    u8 DMA;             // channel, 0xFF for an instruction
    int Range;
};

enum class ActionKind : u8
{
    Return = 1,         // r0 = Value (r1 = Value2 with Action_SetR1), then PC = LR
    SetReg = 2,         // register Reg = Value (15: jump, keeping the ARM/Thumb state)
    Write = 3,          // write Size (1, 2, 4) bytes of Value at Target, through the CPU's bus
    Skip = 4,           // skip Size instructions (PC += Size * instruction size)
};
enum : u32 { Action_SetR1 = 1 };

struct Action
{
    int ID;
    u8 CPUMask;
    u32 Addr;
    ActionKind Kind;
    s8 Reg;
    u32 Value, Value2, Target, Size;
    u32 LR;             // only hits whose LR (bit 0 cleared) is this; 0 = any
    u32 First;          // fire from the First-th matching hit (1 = the first)
    u32 Times;          // fire at most this many times; 0 = no limit
    u32 Flags;
    u64 Matched, Fired;
};

struct ActionRecord
{
    u64 Seq, Frame, Cycle;
    int ActionID;
    u8 CPU;
    ActionKind Kind;
    u64 Hit;            // which matching hit this was (1 = the first)
    u32 PC, LR;
    std::array<u32, 4> Regs;    // r0-r3 before the action
    u32 Value, Target;
};

struct ProbeStats
{
    u64 Recorded = 0, Dropped = 0;
};

class DebugProbes
{
public:
    DebugProbes(NDS& nds, Debugger& dbg) noexcept;

    // The frontend's frame counter (mds_frames), stamped on every record; null stamps 0.
    const u64* FrameCounter = nullptr;

    // Function probes. Add returns the id (> 0), or -1 (bad register or read length).
    int AddProbe(const Probe& probe);
    bool RemoveProbe(int id);
    const std::vector<Probe>& GetProbes() const { return Probes; }
    void SetProbeCapacity(size_t records);  // empties the ring (default 65536)
    std::vector<ProbeRecord> DrainProbes();
    ProbeStats ProbeCounts() const { return ProbeStat; }
    size_t PendingReturns() const { return Pending[0].size() + Pending[1].size(); }
    size_t ProbesInRing() const { return ProbeRing.size(); }

    // Memory access log.
    int AddMemLog(const MemLogRange& range);
    bool RemoveMemLog(int id);
    const std::vector<MemLogRange>& GetMemLogs() const { return MemLogs; }
    void SetMemLogCapacity(size_t records);     // empties the ring (default 1 << 20)
    std::vector<MemLogRecord> DrainMemLog();
    ProbeStats MemLogCounts() const { return MemLogStat; }
    size_t MemLogInRing() const { return MemRing.size(); }

    // Breakpoint actions. Add returns the id, or -1 (bad kind, register or size).
    int AddAction(const Action& action);
    bool RemoveAction(int id);
    const std::vector<Action>& GetActions() const { return Actions; }
    std::vector<ActionRecord> DrainActions();
    // Recorded: every action applied since the console was created (never reset, so a run can be marked).
    ProbeStats ActionCounts() const { return ActionStat; }
    size_t ActionsInRing() const { return ActionRing.size(); }

    // Overlay attribution and tagged call counts / coverage.
    int OverlayAt(u32 addr);
    void SetTagging(bool enabled);
    bool Tagging() const { return Tags; }
    std::vector<std::pair<std::pair<u32, int>, u64>> DrainCallCountsTagged(u8 cpu);
    std::vector<std::pair<u32, int>> DrainCoverageTagged(u8 cpu);

    // Hooks (Debugger).
    bool InstrArmed(u8 cpu) const { return InstrHook[cpu]; }
    bool DataArmed(u8 cpu) const { return DataHook[cpu]; }
    // True when an action moved PC: the instruction at pc does not run and the hooks run again at the new PC.
    bool OnInstr(ARM& cpu, u32 pc);
    void OnData(u8 cpu, u32 addr, u8 size, u32 value, bool write, u32 pc);
    void OnDMA(u8 cpu, u8 channel, u32 src, u32 dst, u8 size, u32 value);
    void CountCall(u8 cpu, u32 entry);
    void Cover(u8 cpu, u32 pc);
    void Reset();           // a reset or ROM load: pending returns and the overlay table go
    void OnStateLoaded();   // pending returns go; probes, ranges, actions and logs stay
    void Rearm();           // recompute InstrHook / DataHook (then Debugger::Rearm)

private:
    melonDS::NDS& NDS;
    Debugger& D;
    int NextID = 1;

    std::vector<Probe> Probes;
    std::vector<MemLogRange> MemLogs;
    std::vector<Action> Actions;
    // per CPU, per 4 KiB page: bit 0 a probe, bit 1 an action starts there; bit 2 a logged range covers it
    std::array<std::vector<u8>, 2> ExecPages, DataPages;
    std::array<bool, 2> InstrHook {}, DataHook {};

    struct PendingReturn
    {
        u32 Ret, SP;
        u8 Mode;
        u64 Seq;            // the call record's sequence number
        u64 Slot;           // its push index in the ring
        int ProbeID;
        std::array<u32, ProbeReads> Bases;
    };
    std::array<std::vector<PendingReturn>, 2> Pending;
    std::array<std::array<u16, 1024>, 2> PendingFilter {};

    std::deque<ProbeRecord> ProbeRing;
    size_t ProbeCapacity = 65536;
    u64 ProbePushed = 0, ProbePopped = 0, NextCall = 0;
    ProbeStats ProbeStat;

    std::deque<MemLogRecord> MemRing;
    size_t MemCapacity = 1 << 20;
    u64 NextAccess = 0;
    ProbeStats MemLogStat;

    std::deque<ActionRecord> ActionRing;
    u64 NextFired = 0;
    ProbeStats ActionStat;

    // overlay attribution
    bool OverlaysRead = false;
    std::vector<RomOverlay> Overlays;
    struct Region
    {
        u32 Start, End;                 // code range covered by the members (end exclusive)
        u32 Base;                       // the lowest member base
        std::vector<int> Members;
    };
    std::vector<Region> Regions;
    struct CacheEntry
    {
        u32 Addr = 1;                   // odd: empty
        int Id = OverlayStatic;
        std::array<u32, 4> AtAddr {}, AtBase {};
    };
    std::vector<CacheEntry> Cache;

    bool Tags = false;
    std::array<std::unordered_map<u64, u64>, 2> CallsTagged;
    std::array<std::unordered_map<u64, std::array<u64, 32>>, 2> CoverTagged;
    std::array<u32, 2> CoverLo {}, CoverHi {};
    std::array<std::array<u64, 32>*, 2> CoverBits {};
    std::array<int, 2> CoverOverlay {};

    u64 Frame() const { return FrameCounter ? *FrameCounter : 0; }
    u64 Cycle(u8 cpu) const;
    u32 Read32(u32 addr) const;
    void ReadBytes(u8 cpu, u32 addr, u32 len, std::vector<u8>& out) const;
    void RebuildPages();
    void LoadOverlays();
    const Region* RegionAt(u32 addr) const;
    void CheckReturns(ARM& cpu, u32 pc);
    void RecordEntry(ARM& cpu, Probe& p, u32 pc, std::vector<u64>& slots);
    bool ApplyAction(ARM& cpu, Action& a, u32 pc);
    void PushProbe(ProbeRecord&& rec);
    void DropPending(u8 n, size_t index);
};

}

#endif
