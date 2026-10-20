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

MDS_API int mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states);
MDS_API int mds_wait(const int* ids, int count, int timeout_ms, int* states);
MDS_API int mds_gdb(int id, int port9, int port7, int break_on_start);
MDS_API int mds_start(int id, uint32_t frames);
MDS_API int mds_stop_after(int id, uint32_t frames);
MDS_API int mds_interrupt(int id);
MDS_API int mds_state(int id);
MDS_API int mds_take_break(int id, mds_break* out);
MDS_API uint64_t mds_frames(int id);

MDS_API int mds_screen(int id, uint32_t* out);
MDS_API int mds_set_keys(int id, uint32_t pressed);
MDS_API int mds_touch(int id, int x, int y);
MDS_API int mds_set_lid(int id, int closed);

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
MDS_API int mds_threadstacks(int id, int cpu, uint32_t* sps, uint32_t* depths, int max_stacks, mds_frame* out, int max_frames);
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

#ifdef __cplusplus
}
#endif

#endif
