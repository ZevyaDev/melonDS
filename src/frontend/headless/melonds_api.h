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

MDS_API int mds_version(void);
MDS_API const char* mds_error(void);
MDS_API void mds_set_local_dir(const char* dir);
MDS_API void mds_set_log_level(int level);
MDS_API int mds_mp_timeout(int ms);
MDS_API void mds_set_pacing(int fps);

MDS_API int mds_create(const char* bios9, const char* bios7, const char* firmware, const char* nickname, int debug);
MDS_API void mds_destroy(int id);
MDS_API int mds_load_rom(int id, const char* rom, const char* save);
MDS_API int mds_reset(int id);
// Clock as year (2000-2099), month, day, hour, minute, second. Needs a stopped console.
MDS_API int mds_set_rtc(int id, int year, int month, int day, int hour, int minute, int second);
MDS_API int mds_get_rtc(int id, int* out);

MDS_API int mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states);
MDS_API int mds_wait(const int* ids, int count, int timeout_ms, int* states);
MDS_API int mds_gdb(int id, int port9, int port7, int break_on_start);
MDS_API int mds_start(int id, uint32_t frames);
MDS_API int mds_stop_after(int id, uint32_t frames);
MDS_API int mds_interrupt(int id);
MDS_API int mds_state(int id);
MDS_API int mds_take_break(int id, mds_break* out);
MDS_API uint64_t mds_frames(int id);

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

MDS_API int mds_read(int id, int cpu, uint32_t addr, uint32_t len, uint8_t* out);
MDS_API int mds_write(int id, int cpu, uint32_t addr, uint32_t len, const uint8_t* data);
MDS_API int mds_get_regs(int id, int cpu, uint32_t* out);
MDS_API int mds_set_reg(int id, int cpu, int reg, uint32_t value);

MDS_API int mds_bp_add(int id, uint32_t cpumask, uint32_t addr);
MDS_API int mds_wp_add(int id, uint32_t cpumask, uint32_t addr, uint32_t size, uint32_t flags);
MDS_API int mds_point_remove(int id, int point);
MDS_API int mds_point_enable(int id, int point, int enabled);
MDS_API int mds_point_list(int id, mds_point* out, int max);
MDS_API int mds_step(int id, int cpu, uint32_t count);

MDS_API int mds_trace(int id, uint32_t cpumask, uint32_t start, uint32_t end, uint32_t capacity);
MDS_API int mds_trace_take(int id, mds_trace_entry* out, int max);
MDS_API int mds_calls(int id, int enabled);
MDS_API int mds_callstack(int id, int cpu, mds_frame* out, int max);
MDS_API int mds_callcounts_take(int id, int cpu, uint32_t* entries, uint64_t* counts, int max);
MDS_API int mds_coverage(int id, int enabled);
MDS_API int mds_coverage_take(int id, int cpu, uint32_t* out, int max);
MDS_API int mds_lastwriter(int id, int enabled);
MDS_API uint32_t mds_lastwriter_get(int id, uint32_t addr);

MDS_API int mds_savestate(int id, const char* path);
MDS_API int mds_loadstate(int id, const char* path);

MDS_API int mds_lua_run(int id, const char* source, int is_file);
MDS_API int mds_lua_exec(int id, const char* code);
MDS_API int mds_lua_stop(int id);
MDS_API int mds_lua_output(int id, char* out, int max);

MDS_API int mds_packets(int id, int enabled, int capacity);
MDS_API int mds_packets_take(int id, mds_packet* headers, uint8_t* data, int max_packets, int max_data);

// Socket server (protocol: Protocol.h). One server thread per process serves every console.
// port 0 lets the OS pick; otherwise the first free port in [port, port + range) is used,
// where range is flags & 0xFFFF (0 = 32, 1 = strict). Returns 0 and the bound port, or -1
// (mds_error: "no free port in A-B", "server already running").
MDS_API int mds_serve(int port, int flags, int* bound);
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
