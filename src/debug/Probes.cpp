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

#include "Probes.h"
#include "Debugger.h"
#include "RomOverlays.h"
#include "../ARM.h"
#include "../NDS.h"

namespace melonDS
{

constexpr u32 PageShift = 12;
constexpr u32 PageCount = 1u << (32 - PageShift);
constexpr size_t MaxPending = 256;          // per CPU: calls whose return is still awaited
constexpr size_t ActionRingSize = 65536;
constexpr size_t CacheSize = 4096;          // overlay attribution cache entries (a power of two)
enum : u8 { Page_Probe = 1, Page_Action = 2 };

// ---------------------------------------------------------------------------------------------------------------

DebugProbes::DebugProbes(melonDS::NDS& nds, Debugger& dbg) noexcept : NDS(nds), D(dbg)
{
}

u64 DebugProbes::Cycle(u8 cpu) const
{
    return cpu ? NDS.ARM7Timestamp : (NDS.ARM9Timestamp >> NDS.ARM9ClockShift);
}

static bool InTCM(const melonDS::NDS& nds, u32 addr)
{
    return addr < nds.ARM9->ITCMSize || (addr & nds.ARM9->DTCMMask) == nds.ARM9->DTCMBase;
}

u32 DebugProbes::Read32(u32 addr) const
{
    addr &= ~3u;
    if ((addr >> 24) == 0x02 && !InTCM(NDS, addr))
    {
        u32 v;
        memcpy(&v, &NDS.MainRAM[addr & NDS.MainRAMMask], 4);
        return v;
    }
    return D.Peek(0, addr, 4);
}

void DebugProbes::ReadBytes(u8 cpu, u32 addr, u32 len, std::vector<u8>& out) const
{
    for (u32 i = 0; i < len; i++)
    {
        u32 a = addr + i;
        if ((a >> 24) == 0x02 && (cpu || !InTCM(NDS, a)))
            out.push_back(NDS.MainRAM[a & NDS.MainRAMMask]);
        else
            out.push_back(D.Peek(cpu, a, 1));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Configuration

int DebugProbes::AddProbe(const Probe& probe)
{
    for (const ProbeRead& r : probe.Reads)
        if (r.Reg < -1 || r.Reg > 15 || r.Len > ProbeMaxRead || (r.Reg >= 0 && r.Len && !(r.When & 3)))
            return -1;
    Probe p = probe;
    p.ID = NextID++;
    p.Addr &= ~1u;
    p.CPUMask &= 3;
    p.Hits = 0;
    Probes.push_back(p);
    RebuildPages();
    return p.ID;
}

bool DebugProbes::RemoveProbe(int id)
{
    auto it = std::find_if(Probes.begin(), Probes.end(), [id](const Probe& p) { return p.ID == id; });
    if (it == Probes.end())
        return false;
    Probes.erase(it);
    for (u8 n = 0; n < 2; n++)
        for (size_t i = Pending[n].size(); i-- > 0;)
            if (Pending[n][i].ProbeID == id)
                DropPending(n, i);
    RebuildPages();
    return true;
}

void DebugProbes::SetProbeCapacity(size_t records)
{
    ProbeCapacity = std::max<size_t>(records, 1);
    ProbePopped += ProbeRing.size();
    ProbeRing.clear();
}

std::vector<ProbeRecord> DebugProbes::DrainProbes()
{
    std::vector<ProbeRecord> out(std::make_move_iterator(ProbeRing.begin()), std::make_move_iterator(ProbeRing.end()));
    ProbePopped += ProbeRing.size();
    ProbeRing.clear();
    return out;
}

int DebugProbes::AddMemLog(const MemLogRange& range)
{
    if (!(range.Flags & (MemLog_Read | MemLog_Write)) || range.End < range.Start)
        return -1;
    MemLogRange r = range;
    r.ID = NextID++;
    r.CPUMask &= 3;
    r.Flags &= 7;
    r.Hits = 0;
    MemLogs.push_back(r);
    RebuildPages();
    return r.ID;
}

bool DebugProbes::RemoveMemLog(int id)
{
    auto it = std::find_if(MemLogs.begin(), MemLogs.end(), [id](const MemLogRange& r) { return r.ID == id; });
    if (it == MemLogs.end())
        return false;
    MemLogs.erase(it);
    RebuildPages();
    return true;
}

void DebugProbes::SetMemLogCapacity(size_t records)
{
    MemCapacity = std::max<size_t>(records, 1);
    MemRing.clear();
}

std::vector<MemLogRecord> DebugProbes::DrainMemLog()
{
    std::vector<MemLogRecord> out(MemRing.begin(), MemRing.end());
    MemRing.clear();
    return out;
}

int DebugProbes::AddAction(const Action& action)
{
    switch (action.Kind)
    {
    case ActionKind::Return:
        break;
    case ActionKind::SetReg:
        if (action.Reg < 0 || action.Reg > 15)
            return -1;
        break;
    case ActionKind::Write:
        if ((action.Size != 1 && action.Size != 2 && action.Size != 4) || (action.Target >> 24) == 0x04)
            return -1;
        break;
    case ActionKind::Skip:
        if (action.Size == 0)
            return -1;
        break;
    default:
        return -1;
    }
    Action a = action;
    a.ID = NextID++;
    a.Addr &= ~1u;
    a.CPUMask &= 3;
    a.LR &= ~1u;
    a.First = std::max<u32>(a.First, 1);
    a.Matched = a.Fired = 0;
    Actions.push_back(a);
    RebuildPages();
    return a.ID;
}

bool DebugProbes::RemoveAction(int id)
{
    auto it = std::find_if(Actions.begin(), Actions.end(), [id](const Action& a) { return a.ID == id; });
    if (it == Actions.end())
        return false;
    Actions.erase(it);
    RebuildPages();
    return true;
}

std::vector<ActionRecord> DebugProbes::DrainActions()
{
    std::vector<ActionRecord> out(ActionRing.begin(), ActionRing.end());
    ActionRing.clear();
    return out;
}

void DebugProbes::RebuildPages()
{
    for (int i = 0; i < 2; i++)
    {
        ExecPages[i].clear();
        DataPages[i].clear();
        auto mark = [&](u32 addr, u8 bit) {
            if (ExecPages[i].empty())
                ExecPages[i].assign(PageCount, 0);
            ExecPages[i][addr >> PageShift] |= bit;
        };
        for (auto& p : Probes)
            if (p.CPUMask & (1 << i))
                mark(p.Addr, Page_Probe);
        for (auto& a : Actions)
            if (a.CPUMask & (1 << i))
                mark(a.Addr, Page_Action);
        for (auto& r : MemLogs)
        {
            if (!(r.CPUMask & (1 << i)))
                continue;
            if (DataPages[i].empty())
                DataPages[i].assign(PageCount, 0);
            for (u64 p = r.Start >> PageShift; p <= (r.End >> PageShift); p++)
                DataPages[i][p] = 1;
        }
    }
    Rearm();
}

void DebugProbes::Rearm()
{
    for (int i = 0; i < 2; i++)
    {
        InstrHook[i] = !ExecPages[i].empty() || !Pending[i].empty();
        DataHook[i] = !DataPages[i].empty();
    }
    D.Rearm();
}

void DebugProbes::Reset()
{
    for (u8 n = 0; n < 2; n++)
    {
        Pending[n].clear();
        PendingFilter[n] = {};
    }
    OverlaysRead = false;
    Overlays.clear();
    Regions.clear();
    Cache.clear();
    CoverBits = {};
}

void DebugProbes::OnStateLoaded()
{
    // the calls awaited ran before the load: their returns will never come
    for (u8 n = 0; n < 2; n++)
        while (!Pending[n].empty())
            DropPending(n, Pending[n].size() - 1);
    CoverBits = {};
}

// ---------------------------------------------------------------------------------------------------------------
// Probes

void DebugProbes::PushProbe(ProbeRecord&& rec)
{
    if (ProbeRing.size() >= ProbeCapacity)
    {
        ProbeRing.pop_front();
        ProbePopped++;
        ProbeStat.Dropped++;
    }
    ProbeRing.push_back(std::move(rec));
    ProbePushed++;
    ProbeStat.Recorded++;
}

void DebugProbes::DropPending(u8 n, size_t index)
{
    PendingReturn& p = Pending[n][index];
    if (p.Slot >= ProbePopped)
        ProbeRing[p.Slot - ProbePopped].Flags |= ProbeRec_ReturnLost;
    PendingFilter[n][(p.Ret >> 1) & 1023]--;
    Pending[n].erase(Pending[n].begin() + index);
}

void DebugProbes::RecordEntry(ARM& cpu, Probe& p, u32 pc, std::vector<u64>& slots)
{
    u8 n = cpu.Num;
    u32 lr = cpu.R[14] & ~1u;
    if ((p.LRStart || p.LREnd) && (lr < p.LRStart || lr > p.LREnd))
        return;
    p.Hits++;
    ProbeRecord rec {};
    rec.Seq = NextCall++;
    rec.Frame = Frame();
    rec.Cycle = Cycle(n);
    rec.ProbeID = p.ID;
    rec.CPU = n;
    rec.Overlay = n == 0 ? OverlayAt(pc) : OverlayStatic;
    rec.Depth = D.CallDepth(n);
    rec.Addr = pc;
    rec.LR = cpu.R[14];
    rec.SP = cpu.R[13];
    rec.Args = {cpu.R[0], cpu.R[1], cpu.R[2], cpu.R[3]};
    std::array<u32, ProbeReads> bases {};
    for (int i = 0; i < ProbeReads; i++)
    {
        const ProbeRead& r = p.Reads[i];
        if (r.Reg < 0 || !r.Len)
            continue;
        bases[i] = (r.Reg == 15 ? pc : cpu.R[r.Reg]) + r.Offset;
        if (r.When & ProbeRead_Entry)
        {
            ReadBytes(n, bases[i], r.Len, rec.Data);
            rec.EntryLen[i] = r.Len;
        }
    }
    u64 slot = ProbePushed;
    u64 seq = rec.Seq;
    PushProbe(std::move(rec));
    slots.push_back(slot);
    if (p.Flags & Probe_NoReturn)
        return;
    if (Pending[n].size() >= MaxPending)
        DropPending(n, 0);  // the oldest: as far as can be told, that call never returned
    Pending[n].push_back({lr, cpu.R[13], (u8)(cpu.CPSR & 0x1F), seq, slot, p.ID, bases});
    PendingFilter[n][(lr >> 1) & 1023]++;
    InstrHook[n] = true;
}

void DebugProbes::CheckReturns(ARM& cpu, u32 pc)
{
    u8 n = cpu.Num;
    auto& pending = Pending[n];
    u32 sp = cpu.R[13];
    u8 mode = cpu.CPSR & 0x1F;
    // innermost first: a recursive call returns to the same address on a lower stack
    for (size_t i = pending.size(); i-- > 0;)
    {
        PendingReturn& p = pending[i];
        if (p.Ret != pc || p.SP != sp || p.Mode != mode)
            continue;
        auto probe = std::find_if(Probes.begin(), Probes.end(), [&](const Probe& q) { return q.ID == p.ProbeID; });
        ProbeRecord late {};
        ProbeRecord* rec = &late;
        if (p.Slot >= ProbePopped)
            rec = &ProbeRing[p.Slot - ProbePopped];
        else
        {
            late.Seq = p.Seq;
            late.Kind = 1;
            late.ProbeID = p.ProbeID;
            late.CPU = n;
            late.Overlay = OverlayStatic;
            late.Depth = -1;
            late.Addr = probe != Probes.end() ? probe->Addr : 0;
            late.LR = p.Ret;
            late.SP = p.SP;
        }
        rec->Returned = true;
        rec->Ret = {cpu.R[0], cpu.R[1]};
        rec->RetFrame = Frame();
        rec->RetCycle = Cycle(n);
        if (probe != Probes.end())
            for (int r = 0; r < ProbeReads; r++)
            {
                const ProbeRead& rd = probe->Reads[r];
                if (rd.Reg < 0 || !rd.Len || !(rd.When & ProbeRead_Return))
                    continue;
                ReadBytes(n, p.Bases[r], rd.Len, rec->Data);
                rec->RetLen[r] = rd.Len;
            }
        PendingFilter[n][(p.Ret >> 1) & 1023]--;
        pending.erase(pending.begin() + i);
        if (rec == &late)
            PushProbe(std::move(late));
        if (pending.empty() && ExecPages[n].empty())
            Rearm();
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Actions

bool DebugProbes::ApplyAction(ARM& cpu, Action& a, u32 pc)
{
    u8 n = cpu.Num;
    if (a.LR && (cpu.R[14] & ~1u) != a.LR)
        return false;
    a.Matched++;
    if (a.Matched < a.First || (a.Times && a.Fired >= a.Times))
        return false;
    a.Fired++;
    ActionRecord rec {NextFired++, Frame(), Cycle(n), a.ID, n, a.Kind, a.Matched, pc, cpu.R[14],
                      {cpu.R[0], cpu.R[1], cpu.R[2], cpu.R[3]}, a.Value, a.Target};
    bool thumb = cpu.CPSR & 0x20;
    switch (a.Kind)
    {
    case ActionKind::Return:
        cpu.R[0] = a.Value;
        if (a.Flags & Action_SetR1)
            cpu.R[1] = a.Value2;
        cpu.JumpTo(cpu.R[14]);  // bit 0 of LR picks Thumb, as BX LR does
        break;
    case ActionKind::SetReg:
        if (a.Reg == 15)
            cpu.JumpTo(a.Value | (thumb ? 1 : 0));
        else
            cpu.R[a.Reg] = a.Value;
        break;
    case ActionKind::Write:
        D.PokeFromHook(n, a.Target, a.Size, a.Value);
        break;
    case ActionKind::Skip:
        cpu.JumpTo((pc + a.Size * (thumb ? 2 : 4)) | (thumb ? 1 : 0));
        break;
    }
    if (ActionRing.size() >= ActionRingSize)
    {
        ActionRing.pop_front();
        ActionStat.Dropped++;
    }
    ActionRing.push_back(rec);
    ActionStat.Recorded++;
    return true;
}

bool DebugProbes::OnInstr(ARM& cpu, u32 pc)
{
    u8 n = cpu.Num;
    if (!Pending[n].empty() && PendingFilter[n][(pc >> 1) & 1023])
        CheckReturns(cpu, pc);
    if (ExecPages[n].empty())
        return false;
    u8 page = ExecPages[n][pc >> PageShift];
    if (!page)
        return false;
    std::vector<u64> slots;
    if (page & Page_Probe)
        for (auto& p : Probes)
            if (p.Addr == pc && (p.CPUMask & (1 << n)))
                RecordEntry(cpu, p, pc, slots);
    if (!(page & Page_Action))
        return false;
    u32 r15 = cpu.R[15], cpsr = cpu.CPSR;
    bool fired = false;
    for (size_t i = 0; i < Actions.size(); i++)
    {
        Action& a = Actions[i];
        if (a.Addr != pc || !(a.CPUMask & (1 << n)))
            continue;
        fired |= ApplyAction(cpu, a, pc);
        if (cpu.R[15] != r15 || ((cpu.CPSR ^ cpsr) & 0x20))
            break;  // PC moved: the actions behind this one belong to an instruction that does not run
    }
    if (fired)
        for (u64 slot : slots)
            if (slot >= ProbePopped)
                ProbeRing[slot - ProbePopped].Flags |= ProbeRec_Forced;
    return cpu.R[15] != r15 || ((cpu.CPSR ^ cpsr) & 0x20);
}

// ---------------------------------------------------------------------------------------------------------------
// Memory access log

void DebugProbes::OnData(u8 n, u32 addr, u8 size, u32 value, bool write, u32 pc)
{
    if (DataPages[n].empty() || !DataPages[n][addr >> PageShift])
        return;
    u8 kind = write ? MemLog_Write : MemLog_Read;
    u32 last = addr + size - 1;
    for (auto& r : MemLogs)
    {
        if (!(r.CPUMask & (1 << n)) || !(r.Flags & kind) || addr > r.End || last < r.Start)
            continue;
        if ((r.PCStart || r.PCEnd) && (pc < r.PCStart || pc > r.PCEnd))
            continue;
        r.Hits++;
        if (!write)
            value = D.Peek(n, addr, size);     // the hook runs before the read: this is what it reads
        if (MemRing.size() >= MemCapacity)
        {
            MemRing.pop_front();
            MemLogStat.Dropped++;
        }
        MemRing.push_back({NextAccess++, Frame(), Cycle(n), pc, addr, value, n, size, kind, 0xFF, r.ID});
        MemLogStat.Recorded++;
        return;     // one record per access: the first range that takes it
    }
}

void DebugProbes::OnDMA(u8 n, u8 channel, u32 src, u32 dst, u8 size, u32 value)
{
    if (DataPages[n].empty())
        return;
    u32 tag = DMAWriterTag | (channel << 1) | n;
    for (int pass = 0; pass < 2; pass++)
    {
        u32 addr = pass ? dst : src;
        u8 kind = pass ? MemLog_Write : MemLog_Read;
        if (!DataPages[n][addr >> PageShift])
            continue;
        for (auto& r : MemLogs)
        {
            if (!(r.CPUMask & (1 << n)) || !(r.Flags & kind) || !(r.Flags & MemLog_DMA) || addr > r.End
                || addr + size - 1 < r.Start)
                continue;
            r.Hits++;
            if (MemRing.size() >= MemCapacity)
            {
                MemRing.pop_front();
                MemLogStat.Dropped++;
            }
            MemRing.push_back({NextAccess++, Frame(), Cycle(n), tag, addr, value, n, size, (u8)(kind | MemLog_DMA),
                               channel, r.ID});
            MemLogStat.Recorded++;
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Overlay attribution

void DebugProbes::LoadOverlays()
{
    OverlaysRead = true;
    Overlays = ReadRomOverlays(NDS);
    Regions.clear();
    std::vector<int> order;
    for (int i = 0; i < (int)Overlays.size(); i++)
        if (!Overlays[i].Image.empty())
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return Overlays[a].Base < Overlays[b].Base; });
    for (int i : order)
    {
        const RomOverlay& ov = Overlays[i];
        u32 end = ov.Base + ov.Image.size();
        if (!Regions.empty() && ov.Base < Regions.back().End)
        {
            Regions.back().End = std::max(Regions.back().End, end);
            Regions.back().Members.push_back(i);
        }
        else
            Regions.push_back({ov.Base, end, ov.Base, {i}});
    }
    Cache.assign(CacheSize, {});
}

const DebugProbes::Region* DebugProbes::RegionAt(u32 addr) const
{
    auto it = std::upper_bound(Regions.begin(), Regions.end(), addr, [](u32 a, const Region& r) { return a < r.Start; });
    if (it == Regions.begin())
        return nullptr;
    --it;
    return addr < it->End ? &*it : nullptr;
}

int DebugProbes::OverlayAt(u32 addr)
{
    addr &= ~1u;
    if (!OverlaysRead)
        LoadOverlays();
    const Region* r = RegionAt(addr);
    if (!r)
        return OverlayStatic;
    std::array<u32, 4> at, base;
    for (u32 k = 0; k < 4; k++)
    {
        at[k] = Read32((addr & ~3u) + 4 * k);
        base[k] = Read32(r->Base + 4 * k);
    }
    CacheEntry& e = Cache[(addr >> 1) & (CacheSize - 1)];
    if (e.Addr == addr && e.AtAddr == at && e.AtBase == base)
        return e.Id;
    std::vector<int> candidates;
    for (int m : r->Members)
        if (addr >= Overlays[m].Base && addr < Overlays[m].Base + Overlays[m].Image.size())
            candidates.push_back(m);
    int id = MatchOverlay(Overlays, candidates, addr, [this](u32 x) { return Read32(x); });
    e.Addr = addr;
    e.Id = id;
    e.AtAddr = at;
    e.AtBase = base;
    return id;
}

void DebugProbes::SetTagging(bool enabled)
{
    Tags = enabled;
    CoverBits = {};
}

void DebugProbes::CountCall(u8 n, u32 entry)
{
    int ov = n == 0 ? OverlayAt(entry) : OverlayStatic;
    CallsTagged[n][((u64)(u32)ov << 32) | (entry & ~1u)]++;
}

void DebugProbes::Cover(u8 n, u32 pc)
{
    if (!CoverBits[n] || pc < CoverLo[n] || pc >= CoverHi[n])
    {
        // the range around pc where one attribution holds: its page, cut at every overlay's code start and end
        int ov = n == 0 ? OverlayAt(pc) : OverlayStatic;
        u32 lo = pc & ~((1u << PageShift) - 1);
        u64 hi = (u64)lo + (1u << PageShift);
        if (n == 0)
            for (const RomOverlay& o : Overlays)
            {
                if (o.Image.empty())
                    continue;
                for (u64 cut : {(u64)o.Base, (u64)o.Base + o.Image.size()})
                {
                    if (cut <= pc && cut > lo)
                        lo = cut;
                    if (cut > pc && cut < hi)
                        hi = cut;
                }
            }
        CoverLo[n] = lo;
        CoverHi[n] = hi > 0xFFFFFFFF ? 0xFFFFFFFF : (u32)hi;
        CoverOverlay[n] = ov;
        CoverBits[n] = &CoverTagged[n][((u64)(u32)ov << 32) | (pc >> PageShift)];
    }
    u32 idx = (pc & ((1u << PageShift) - 1)) >> 1;
    (*CoverBits[n])[idx >> 6] |= 1ull << (idx & 63);
}

std::vector<std::pair<std::pair<u32, int>, u64>> DebugProbes::DrainCallCountsTagged(u8 cpu)
{
    std::vector<std::pair<std::pair<u32, int>, u64>> out;
    for (auto& [key, count] : CallsTagged[cpu & 1])
        out.push_back({{(u32)key, (int)(u32)(key >> 32)}, count});
    CallsTagged[cpu & 1].clear();
    return out;
}

std::vector<std::pair<u32, int>> DebugProbes::DrainCoverageTagged(u8 cpu)
{
    std::vector<std::pair<u32, int>> out;
    for (auto& [key, bits] : CoverTagged[cpu & 1])
    {
        u32 page = (u32)key;
        int ov = (int)(u32)(key >> 32);
        for (u32 w = 0; w < bits.size(); w++)
            for (u64 b = bits[w]; b; b &= b - 1)
                out.push_back({(page << PageShift) | (((w << 6) | __builtin_ctzll(b)) << 1), ov});
    }
    std::sort(out.begin(), out.end());
    CoverTagged[cpu & 1].clear();
    CoverBits[cpu & 1] = nullptr;
    return out;
}

}
