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

#ifndef MELONDS_API_H
#define MELONDS_API_H

#include <stdint.h>

#ifdef _WIN32
#define MDS_API __declspec(dllexport)
#else
#define MDS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Reference: docs/headless-api.md, section 2 (run states, mds_break) and section 5 (points, trace, frames, packets).
enum
{
    MDS_IDLE = 0,
    MDS_RUNNING = 1,
    MDS_PARKED = 2,
};

typedef struct
{
    int32_t kind;
    int32_t cpu;
    int32_t size;
    int32_t id;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    int32_t dma;
} mds_break;

typedef struct
{
    int32_t id;
    int32_t watch;
    uint32_t cpumask;
    uint32_t flags;
    uint32_t enabled;
    uint32_t start;
    uint32_t end;
    uint64_t hits;
} mds_point;

typedef struct
{
    uint64_t cycle;
    uint32_t pc;
    uint32_t instr;
    uint32_t cpsr;
    uint32_t cpu;
} mds_trace_entry;

typedef struct
{
    uint32_t site;
    uint32_t entry;
    uint32_t ret;
    uint32_t sp;
    uint32_t mode;
    uint32_t exception;
} mds_frame;

typedef struct
{
    uint64_t timestamp;
    uint32_t sent;
    uint32_t type;
    uint32_t length;
} mds_packet;

// Reference: docs/headless-api.md, section 1 (library-wide settings, errors, version and features).
// API level: 1 up to the IPC merge, 2 from fix/api-quirks, 3 from feature/cheats, 4 from merge/upstream-debug
// (MDS_API_VERSION). It goes up whenever functions are added or behaviour changes; mds_has names the individual
// features.
#define MDS_API_VERSION 5
MDS_API int mds_version(void);
// 1 if the library has the named feature or exported function ("serve_host", "mds_serve_host", ...; the list
// is in docs/headless-api.md, section 1), else 0. Absent from libraries before API level 2: look the symbol up.
MDS_API int mds_has(const char* name);
MDS_API const char* mds_error(void);
MDS_API void mds_set_local_dir(const char* dir);
MDS_API void mds_set_log_level(int level);
MDS_API int mds_mp_timeout(int ms);
MDS_API void mds_set_pacing(int fps);

// Reference: docs/headless-api.md, section 2 (lifecycle, save files, reset, clock).
MDS_API int mds_create(const char* bios9, const char* bios7, const char* firmware, const char* nickname, int debug);
MDS_API void mds_destroy(int id);
MDS_API int mds_load_rom(int id, const char* rom, const char* save);
MDS_API int mds_reset(int id);
// Clock as year (2000-2099), month, day, hour, minute, second. Needs a stopped console.
MDS_API int mds_set_rtc(int id, int year, int month, int day, int hour, int minute, int second);
MDS_API int mds_get_rtc(int id, int* out);

// Reference: docs/headless-api.md, section 2 (running, breaks, stepping) and section 8 (mds_gdb).
MDS_API int mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states);
MDS_API int mds_wait(const int* ids, int count, int timeout_ms, int* states);
MDS_API int mds_gdb(int id, int port9, int port7, int break_on_start);
// mds_gdb listening on host (an IPv4 address; NULL or "" = 127.0.0.1, "0.0.0.0" = every interface).
MDS_API int mds_gdb_host(int id, const char* host, int port9, int port7, int break_on_start);
MDS_API int mds_start(int id, uint32_t frames);
MDS_API int mds_stop_after(int id, uint32_t frames);
MDS_API int mds_interrupt(int id);
MDS_API int mds_state(int id);
MDS_API int mds_take_break(int id, mds_break* out);
MDS_API uint64_t mds_frames(int id);

// Reference: docs/headless-api.md, section 3 (input queue, touch timeline, mic, input log).
typedef struct
{
    uint32_t offset;    // system cycles from frame start
    uint16_t x, y;
    uint8_t down;
    uint8_t pad[3];
} mds_touch_point;

typedef struct
{
    uint32_t seq;
    uint16_t buttons;       // 12 bits, same order as mds_set_keys
    uint8_t lid;            // 1 = closed
    uint8_t touch_count;    // 0 = pen unchanged
    uint8_t mic_kind;       // 0 none, 1 samples, 2 level
    uint8_t flags;          // bit 0: count the payload as a socket client's (the repeat-mode stall timeout applies)
    uint16_t mic_count;
    uint16_t mic_level;
    uint16_t pad2;
} mds_input;

typedef struct
{
    uint64_t frame;
    uint32_t seq;           // 0 for in-process sources
    uint16_t buttons;
    uint8_t lid;
    uint8_t touch_count;
    uint8_t mic_kind;
    uint8_t pad;
    uint16_t mic_level;
    uint16_t pad2;
    uint16_t pad3;
} mds_input_log_entry;

// Reference: docs/headless-api.md, section 4 (mds_screen) and section 3 (setters and queued input).
MDS_API int mds_screen(int id, uint32_t* out);
MDS_API int mds_set_keys(int id, uint32_t pressed);
MDS_API int mds_touch(int id, int x, int y);
MDS_API int mds_set_lid(int id, int closed);

// Queue one frame of input. Returns the queue depth, or -1 (mds_error: bad_payload / input_queue_full).
MDS_API int mds_input_submit(int id, const mds_input* in, const mds_touch_point* points, const int16_t* mic);
// Queue one payload for each of count consoles as a barrier group (input_multi): all or none are queued, and
// every console waits at its payload's frame boundary until all of them are there. inputs[i] is console ids[i]'s
// header; points[i] / mic[i] are its touch points and samples (the arrays, or individual entries, may be null when
// the counts are 0). depths, if not null, receives each console's queue depth. Returns 0, or -1 (mds_error:
// no console / bad_payload / input_queue_full).
MDS_API int mds_input_submit_multi(const int* ids, int count, const mds_input* inputs,
                                   const mds_touch_point* const* points, const int16_t* const* mic, int* depths);
// Test hook: mark the console as blocked in (pending 1) or finished with (0) a wireless receive, as the platform
// layer does around a blocking MP receive. A console waiting at a barrier is released while a partner is marked.
MDS_API int mds_debug_recv_pending(int id, int pending);
// mode: 0 repeat, 1 neutral, 2 wait, -1 unchanged. stall_frames: -1 unchanged, 0 never.
MDS_API int mds_input_mode(int id, int mode, int stall_frames);
MDS_API int mds_input_depth(int id);
MDS_API int mds_input_waiting(int id);
MDS_API int mds_input_clear(int id);
MDS_API int mds_input_log(int id, uint64_t from_frame, mds_input_log_entry* out, int max);

// Reference: docs/headless-api.md, section 4 (memory and registers).
MDS_API int mds_read(int id, int cpu, uint32_t addr, uint32_t len, uint8_t* out);
MDS_API int mds_write(int id, int cpu, uint32_t addr, uint32_t len, const uint8_t* data);
MDS_API int mds_get_regs(int id, int cpu, uint32_t* out);
MDS_API int mds_set_reg(int id, int cpu, int reg, uint32_t value);

// Reference: docs/headless-api.md, section 5 (breakpoints, watchpoints) and section 2 (mds_step).
MDS_API int mds_bp_add(int id, uint32_t cpumask, uint32_t addr);
MDS_API int mds_wp_add(int id, uint32_t cpumask, uint32_t addr, uint32_t size, uint32_t flags);
MDS_API int mds_point_remove(int id, int point);
MDS_API int mds_point_enable(int id, int point, int enabled);
MDS_API int mds_point_list(int id, mds_point* out, int max);
MDS_API int mds_step(int id, int cpu, uint32_t count);

// Reference: docs/headless-api.md, section 5 (trace, call tracking, coverage, last writer).
MDS_API int mds_trace(int id, uint32_t cpumask, uint32_t start, uint32_t end, uint32_t capacity);
MDS_API int mds_trace_take(int id, mds_trace_entry* out, int max);
MDS_API int mds_calls(int id, int enabled);
MDS_API int mds_callstack(int id, int cpu, mds_frame* out, int max);
// The stacks of the threads that are not running (API level 4, "thread_stacks"): up to max_stacks stacks, their
// SPs in sps and frame counts in depths, the frames of all of them packed into out (innermost first per stack, at
// most max_frames in total). Returns the number of stacks.
MDS_API int mds_threadstacks(int id, int cpu, uint32_t* sps, uint32_t* depths, int max_stacks, mds_frame* out, int max_frames);
MDS_API int mds_callcounts_take(int id, int cpu, uint32_t* entries, uint64_t* counts, int max);
MDS_API int mds_coverage(int id, int enabled);
MDS_API int mds_coverage_take(int id, int cpu, uint32_t* out, int max);
MDS_API int mds_lastwriter(int id, int enabled);
// Legacy: returns the raw record, and 0 for "no record" and for every error alike.
MDS_API uint32_t mds_lastwriter_get(int id, uint32_t addr);
// The last writer of the main-RAM word at addr: returns 1 (raw = the record: pc | arm7 for an instruction,
// 0xFFFFFFF0 | channel << 1 | arm7 for DMA; dma = the channel, or -1 for an instruction), 0 for no record
// (raw = 0, dma = -1), or -1 (mds_error: unknown console, running, no debugger, tracking off). Either
// pointer may be NULL.
MDS_API int mds_lastwriter_query(int id, uint32_t addr, uint32_t* raw, int* dma);
// The ARM9 overlays of the cart that are resident now (API level 5, "rom_overlays"): what the GDB stub's
// "monitor overlays" reports. Writes the ids, base addresses and sizes (code + bss) of up to max of them, in the
// ROM's overlay-table order; any array may be NULL. Returns how many are resident (which may exceed max), 0
// without a ROM. Needs a stopped console, not the debugger.
MDS_API int mds_overlays(int id, int* ids, uint32_t* bases, uint32_t* sizes, int max);

// Reference: docs/headless-api.md, section 6 (savestates).
MDS_API int mds_savestate(int id, const char* path);
MDS_API int mds_loadstate(int id, const char* path);

// Reference: docs/headless-api.md, section 6a (Action Replay cheat codes). API level 3, feature "cheats".
typedef struct
{
    int32_t cheat;      // the code's id in this console's list (1, 2, ...; never reused)
    int32_t enabled;
    int32_t words;      // code words (two per "XXXXXXXX YYYYYYYY" line)
    char name[116];     // NUL-terminated, cut short if longer (mds_cheat_get has the whole name)
} mds_cheat;

// Add a code to the console's list. code: Action Replay text, one "XXXXXXXX YYYYYYYY" line per pair of words;
// it is checked before anything is added (mds_error: "cheat: line 3: ..."). name may be NULL ("").
// Returns the code's id (>= 1). Any console state; a running console adds it at the next frame boundary.
MDS_API int mds_cheat_add(int id, const char* name, const char* code, int enabled);
MDS_API int mds_cheat_remove(int id, int cheat);
MDS_API int mds_cheat_enable(int id, int cheat, int enabled);
// Remove every code; returns how many there were.
MDS_API int mds_cheat_clear(int id);
// Copy up to max entries in list order; returns the number copied.
MDS_API int mds_cheat_list(int id, mds_cheat* out, int max);
// One code's whole name (into name, name_max bytes with the NUL; may be NULL with name_max 0) and words
// (up to max_words into words). Returns the code's word count, which may exceed max_words.
MDS_API int mds_cheat_get(int id, int cheat, char* name, int name_max, uint32_t* words, int max_words);

// Reference: docs/headless-api.md, section 7 (Lua).
MDS_API int mds_lua_run(int id, const char* source, int is_file);
MDS_API int mds_lua_exec(int id, const char* code);
MDS_API int mds_lua_stop(int id);
MDS_API int mds_lua_output(int id, char* out, int max);

// Reference: docs/headless-api.md, section 5 (wireless packet capture).
MDS_API int mds_packets(int id, int enabled, int capacity);
MDS_API int mds_packets_take(int id, mds_packet* headers, uint8_t* data, int max_packets, int max_data);

// Reference: docs/headless-api.md, section 8 (socket server, instance base).
// Socket server (protocol: Protocol.h). One server thread per process serves every console, on 127.0.0.1.
// port 0 lets the OS pick; otherwise the first free port in [port, port + range) is used,
// where range is flags & 0xFFFF (0 = 32, 1 = strict). Returns 0 and the bound port, or -1
// (mds_error: "no free port in A-B", "server already running").
MDS_API int mds_serve(int port, int flags, int* bound);
// mds_serve listening on host (an IPv4 address; NULL or "" = 127.0.0.1, the default; "0.0.0.0" = every
// interface, which prints a warning: anyone who can reach the port controls the emulator).
MDS_API int mds_serve_host(const char* host, int port, int flags, int* bound);
// Stops the thread, closes the sockets and drops the client. Consoles keep their state.
MDS_API void mds_serve_stop(void);
// Waits up to timeout_ms (< 0 = forever) for a client's `shutdown` message: 1 if one was
// received since mds_serve, else 0. The owner then calls mds_serve_stop and exits.
MDS_API int mds_serve_wait(int timeout_ms);
// Name returned in the hello reply (default "melonds"). Call before or after mds_serve.
MDS_API void mds_serve_name(const char* name);

// Process-wide offset added to every console's id for MAC address and save-file numbering
// (and reported by emu.getconsoleid()), so separate processes stay distinct. Default 0.
// Must be set before any console is created: returns -1 (mds_error) if one exists or base < 0.
MDS_API int mds_set_instance_base(int base);
MDS_API int mds_instance_base(void);
// Firmware MAC address of the console (6 bytes). Save file the console's last mds_load_rom
// resolved to (returns its length, -1 if out is too small).
MDS_API int mds_mac(int id, uint8_t* out);
MDS_API int mds_save_path(int id, char* out, int max);

#ifdef __cplusplus
}
#endif

#endif
