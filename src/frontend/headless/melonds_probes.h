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

#ifndef MELONDS_PROBES_H
#define MELONDS_PROBES_H

// Analysis tools that log without parking the console (included by melonds_api.h; API level 7).
// Reference: docs/headless-api.md, section 5b. Features for mds_has: "probes", "memlog", "bp_actions",
// "overlay_tags". Every call needs a console created with the debugger and stopped (idle or parked), like the
// other debugger calls; the takes are staged (loop until 0).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Function probes (feature "probes") ----------------------------------------------------------------------
// A probe logs every call of the function at addr: r0-r3, LR, SP, the call depth, the overlay there and up to four
// memory reads behind registers, at entry; and r0, r1 (and the reads asked for at return) when that call returns.
// The return is the first time the CPU reaches the return address (LR at entry) with the stack pointer and mode it
// had at entry. Nothing parks and nothing is written: a run with probes is the same run as one without.

#define MDS_PROBE_READ_ENTRY  1     // mds_probe_read.when: read at entry
#define MDS_PROBE_READ_RETURN 2     //                      read at return (from the register's value at entry)
#define MDS_PROBE_NO_RETURN   1     // mds_probe_spec.flags: do not wait for returns (records have returned = 0)
#define MDS_PROBE_FORCED      1     // mds_probe_record.flags: a breakpoint action fired at this call's entry
#define MDS_PROBE_RETURN_LOST 2     //                         the return was given up (state load, too many awaited)
#define MDS_PROBE_MAX_READ    256   // bytes per read

// Overlay ids of the attribution (mds_probe_record.overlay, the tagged takes, mds_overlay_at); >= 0 is a cart
// overlay id.
#define MDS_OVERLAY_STATIC    (-1)  // no overlay covers the address (the main module, TCM, ...) or ARM7 code
#define MDS_OVERLAY_AMBIGUOUS (-2)  // several overlays match the memory there equally well
#define MDS_OVERLAY_UNKNOWN   (-3)  // in overlays' ranges, but the memory there matches none of them

typedef struct
{
    int32_t reg;        // register 0-15 holding the pointer; -1 = unused
    int32_t offset;     // added to the register's value
    uint32_t len;       // bytes, at most MDS_PROBE_MAX_READ
    uint32_t when;      // MDS_PROBE_READ_ENTRY | MDS_PROBE_READ_RETURN
} mds_probe_read;

typedef struct
{
    uint32_t cpumask;   // 1 ARM9, 2 ARM7
    uint32_t addr;      // the function's first instruction (bit 0 ignored)
    uint32_t lr_start;  // log only calls whose return address (LR, bit 0 cleared) is in [lr_start, lr_end];
    uint32_t lr_end;    //   0, 0 = every call
    uint32_t flags;     // MDS_PROBE_NO_RETURN
    mds_probe_read reads[4];
} mds_probe_spec;

typedef struct
{
    uint64_t seq;           // call number, per console from 0 (a gap: records dropped from a full ring)
    uint64_t frame;         // mds_frames at entry (0 for a late return)
    uint64_t cycle;         // the CPU's cycle at entry (ARM9: system cycles), as mds_trace_entry.cycle
    uint64_t ret_frame;     // at return
    uint64_t ret_cycle;
    int32_t probe;          // the probe's id
    int32_t cpu;            // 9 or 7
    int32_t kind;           // 0 a call; 1 a late return: the call's record was taken before it returned (same seq)
    int32_t overlay;        // ARM9: the overlay at addr at entry, or MDS_OVERLAY_*; ARM7: MDS_OVERLAY_STATIC
    int32_t depth;          // the call tracker's depth at entry (mds_calls on), else -1
    uint32_t addr;          // the function
    uint32_t lr;            // LR at entry (the caller's return address, with its Thumb bit)
    uint32_t sp;            // SP at entry
    uint32_t args[4];       // r0-r3 at entry
    uint32_t returned;      // 1: ret holds r0, r1 at the return
    uint32_t ret[2];
    uint32_t flags;         // MDS_PROBE_FORCED, MDS_PROBE_RETURN_LOST
    uint16_t entry_len[4];  // bytes of read i taken at entry ...
    uint16_t ret_len[4];    // ... and at return; the record's data is the entry reads in order, then the return reads
    uint32_t data_len;      // = the sum of the lengths: this record's bytes in the data buffer of mds_probe_take
    uint32_t pad;
} mds_probe_record;

// Returns the probe's id (> 0), or -1 (mds_error: bad read).
MDS_API int mds_probe_add(int id, const mds_probe_spec* spec);
MDS_API int mds_probe_remove(int id, int probe);
// Up to max probes: their ids, specs and hit counts (any pointer may be NULL). Returns how many there are.
MDS_API int mds_probe_list(int id, int* probes, mds_probe_spec* specs, uint64_t* hits, int max);
// The ring of records holds `capacity` records (default 65536; the oldest are dropped). Empties it.
MDS_API int mds_probe_capacity(int id, int capacity);
// Moves up to max records out, oldest first, their data packed into data (max_data bytes). A record whose data does
// not fit is left for the next call, unless it is the first: then its first max_data bytes are returned (data_len
// cut short). Loop until 0.
MDS_API int mds_probe_take(int id, mds_probe_record* out, uint8_t* data, int max, int max_data);

// out[0] records made, out[1] dropped from a full ring, out[2] calls awaiting their return, out[3] in the ring.
// Also for the memory log (mds_memlog_stats: out[2] = 0) and actions (mds_action_stats: out[0] = actions applied
// since the console was created, out[2] = 0).
MDS_API int mds_probe_stats(int id, uint64_t* out4);

// ---- Memory access log (feature "memlog") --------------------------------------------------------------------
// Every read and/or write of the CPUs (and optionally DMA units) that touches a range, logged without parking.

#define MDS_MEMLOG_READ  1
#define MDS_MEMLOG_WRITE 2
#define MDS_MEMLOG_DMA   4      // spec.flags: log DMA units too (source reads, destination writes); record.flags

typedef struct
{
    uint32_t cpumask;   // 1 ARM9, 2 ARM7 (a DMA unit counts for the CPU whose DMA controller it is)
    uint32_t start;
    uint32_t size;      // bytes, > 0
    uint32_t flags;     // MDS_MEMLOG_READ | MDS_MEMLOG_WRITE, | MDS_MEMLOG_DMA
    uint32_t pc_start;  // only accesses made by instructions in [pc_start, pc_end]; 0, 0 = any. DMA units always
    uint32_t pc_end;    //   pass this filter.
} mds_memlog_spec;

typedef struct
{
    uint64_t seq;       // access number, per console from 0 (a gap: records dropped)
    uint64_t frame;     // mds_frames
    uint64_t cycle;
    uint32_t pc;        // the accessing instruction; for DMA the tag 0xFFFFFFF0 | channel << 1 | arm7
    uint32_t addr;
    uint32_t value;     // written, or read (memory read without side effects as the access starts; I/O reads 0)
    int32_t range;      // the id of the first range that took it
    uint8_t cpu;        // 9 or 7
    uint8_t size;       // 1, 2, 4
    uint8_t flags;      // MDS_MEMLOG_READ or MDS_MEMLOG_WRITE, | MDS_MEMLOG_DMA
    uint8_t dma;        // the DMA channel, 0xFF for an instruction
    uint32_t pad;
} mds_memlog_record;

MDS_API int mds_memlog_add(int id, const mds_memlog_spec* spec);   // the range's id, or -1
MDS_API int mds_memlog_remove(int id, int range);
MDS_API int mds_memlog_list(int id, int* ranges, mds_memlog_spec* specs, uint64_t* hits, int max);
MDS_API int mds_memlog_capacity(int id, int capacity);  // default 1 << 20 records; empties the ring
MDS_API int mds_memlog_take(int id, mds_memlog_record* out, int max);
MDS_API int mds_memlog_stats(int id, uint64_t* out4);

// ---- Breakpoint actions (feature "bp_actions") ---------------------------------------------------------------
// At an address, change the run without parking: return early with a value, set a register, write memory or skip
// instructions. Every application is logged (mds_action_take) and counted (mds_action_stats out[0]), so a run that
// used actions can be marked. An action that moves PC (return, skip, set r15) means the instruction at addr does
// not run: the hooks run again at the new PC (a breakpoint there fires); a breakpoint at addr itself does not.

#define MDS_ACTION_RETURN  1    // r0 = value (r1 = value2 with MDS_ACTION_SET_R1), PC = LR
#define MDS_ACTION_SET_REG 2    // register reg = value (15: jump, keeping the ARM/Thumb state)
#define MDS_ACTION_WRITE   3    // write size (1, 2, 4) bytes of value at target through the CPU's bus (not I/O)
#define MDS_ACTION_SKIP    4    // skip size instructions
#define MDS_ACTION_SET_R1  1    // flags

typedef struct
{
    uint32_t cpumask;
    uint32_t addr;
    int32_t kind;       // MDS_ACTION_*
    int32_t reg;        // SET_REG
    uint32_t value;
    uint32_t value2;    // RETURN with MDS_ACTION_SET_R1: r1
    uint32_t target;    // WRITE: the address
    uint32_t size;      // WRITE: bytes; SKIP: instructions
    uint32_t lr;        // only hits whose LR (bit 0 cleared) is lr; 0 = any
    uint32_t first;     // fire from the first-th matching hit (1-based; 0 = 1)
    uint32_t times;     // fire at most this many times (0 = every matching hit from `first`)
    uint32_t flags;
} mds_action_spec;

typedef struct
{
    uint64_t seq;       // application number, per console from 0
    uint64_t frame;
    uint64_t cycle;
    uint64_t hit;       // which matching hit it was (1 = the first)
    int32_t action;
    int32_t cpu;
    int32_t kind;
    uint32_t pc;
    uint32_t lr;
    uint32_t regs[4];   // r0-r3 before the action
    uint32_t value;
    uint32_t target;
    uint32_t pad;
} mds_action_record;

MDS_API int mds_action_add(int id, const mds_action_spec* spec);    // the action's id, or -1
MDS_API int mds_action_remove(int id, int action);
// Up to max actions: ids, specs, matching hits and times fired (any pointer may be NULL). Returns the count.
MDS_API int mds_action_list(int id, int* actions, mds_action_spec* specs, uint64_t* matched, uint64_t* fired, int max);
MDS_API int mds_action_take(int id, mds_action_record* out, int max);   // log of the last 65536 applications
MDS_API int mds_action_stats(int id, uint64_t* out4);

// ---- Overlay tags (feature "overlay_tags") -------------------------------------------------------------------
// The core attributes ARM9 code to the cart overlay whose image matches the memory at the address at that moment
// (no game-specific table). With tags on, call counts and coverage are also kept per (address, overlay); the
// untagged mds_callcounts_take / mds_coverage_take are unchanged.

MDS_API int mds_overlay_tags(int id, int enabled);
// The overlay at addr now (or MDS_OVERLAY_*); errors return INT32_MIN (mds_error set).
MDS_API int mds_overlay_at(int id, uint32_t addr);
MDS_API int mds_callcounts_take_ov(int id, int cpu, uint32_t* entries, int32_t* overlays, uint64_t* counts, int max);
MDS_API int mds_coverage_take_ov(int id, int cpu, uint32_t* addrs, int32_t* overlays, int max);

#ifdef __cplusplus
}
#endif

#endif
