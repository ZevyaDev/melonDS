# melonDS headless C API reference

This is the reference for `src/frontend/headless/melonds_api.h`, the C API exported by
`melonds_headless.dll` (`.so`, `.dylib`). It says what every exported function does, what state it
needs, what it blocks on and what it returns, so it can be used without reading the implementation.

Every claim here was taken from the code (`src/frontend/headless/*`, `src/debug/Debugger.*`, the
CPU and DMA hook sites in `src/ARM*.cpp`, `src/CP15.cpp`, `src/DMA.cpp`) or from the tests
(`src/frontend/headless/tests`, `tools/melonds-mcp/tests`). Claims marked *(inferred)* follow from
reading the code but no test covers them. Claims marked *(verified)* were checked with a small
one-console experiment against this branch's DLL. Bugs and surprises are collected at the end, in
[Known issues and surprises](#11-known-issues-and-surprises).

This revision describes **API level 2**, the `fix/api-quirks` branch (see
[Version and features](#version-and-features) for what changed from level 1).

Contents

1. [Overview](#1-overview)
2. [Lifecycle and running](#2-lifecycle-and-running)
3. [Input](#3-input)
4. [Memory and CPU](#4-memory-and-cpu)
5. [Debugging](#5-debugging)
6. [Savestates](#6-savestates)
7. [Lua](#7-lua)
8. [GDB stub and socket server](#8-gdb-stub-and-socket-server)
9. [Python equivalents](#9-python-equivalents)
10. [Recipes](#10-recipes)
11. [Known issues and surprises](#11-known-issues-and-surprises)

---

## 1. Overview

### What the library is

melonDS without a window. One process can hold up to 16 emulated DS consoles. Each has its own
emulator thread, its own debugger (breakpoints, watchpoints including DMA writes, stepping, traces,
call stacks, call counts, coverage, last writer, wireless packet capture), its own Lua state and
its own input queue. The library also contains a socket server (`mds_serve`) that exposes the same
consoles over TCP. JIT is always off in the headless build (`Instance.cpp`: `args.JIT =
std::nullopt`); the interpreter runs every console.

### Consoles and ids

`mds_create` returns a console id, the lowest free slot in 0-15 ("too many consoles" beyond 16).
Ids are reused after `mds_destroy`. Every other per-console call takes that id; an unknown id fails
with `no console N`.

The id is also the console's slot in the in-process wireless link. A separate *effective id*
(`id + mds_instance_base()`) is used only for the firmware MAC address, the default save-file name
and Lua's `emu.getconsoleid()` (see [section 8](#8-gdb-stub-and-socket-server)).

### Threads and the command queue

Each console owns one emulator thread. Nothing outside that thread touches the console's `NDS`
object mid-frame: calls that need the console are posted to the console's *command queue* and run
by the emulator thread, while the calling thread waits for the result (`Instance::Call`). The
emulator thread drains the queue:

- at every **frame boundary** while running (`Instance::ThreadMain`),
- **at once** when the console is idle (the thread wakes for the command),
- **at once** while the console is parked at a break (`Instance::Park` drains while parked),
- while the console is blocked at a frame boundary **waiting for input** or at an input barrier.

So a call such as `mds_read` on a running console returns at the end of the current frame, and
what it reads is a frame-boundary state, never a mid-frame one (test
`test_reads_while_running_only_see_frame_boundary_states`). `mds_write` on a running console is
applied at the next frame boundary (test `test_write_while_running_applies_at_frame_boundary`).
Calls from several threads at once are safe (test `test_concurrent_calls_while_running`).

The calls fall into three groups by what they need:

| Needs | Calls |
| --- | --- |
| no console | `mds_version`, `mds_has`, `mds_error`, `mds_set_local_dir`, `mds_set_log_level`, `mds_set_pacing`, `mds_mp_timeout` (after the first `mds_create`), `mds_serve*`, `mds_set_instance_base` (no console may exist), `mds_instance_base` |
| any state | `mds_destroy`, `mds_stop_after`, `mds_interrupt` (debugger), `mds_state`, `mds_take_break`, `mds_frames`, `mds_wait`, `mds_screen`, `mds_set_keys`, `mds_touch`, `mds_set_lid`, all `mds_input_*`, `mds_debug_recv_pending`, `mds_read`, `mds_write`, `mds_savestate`/`mds_loadstate` (accepted, but fail unless idle), `mds_lua_output`, `mds_packets`, `mds_packets_take`, `mds_mac`, `mds_save_path` |
| stopped (idle **or** parked; fails with `console N is running`) | `mds_set_rtc`, `mds_get_rtc`, `mds_run`, `mds_start`, `mds_get_regs`, `mds_set_reg`, `mds_lua_run`, `mds_lua_exec`, `mds_lua_stop`, and the debugger calls: `mds_gdb`, `mds_gdb_host`, `mds_bp_add`, `mds_wp_add`, `mds_point_remove`, `mds_point_enable`, `mds_point_list`, `mds_step`, `mds_trace`, `mds_trace_take`, `mds_calls`, `mds_callstack`, `mds_callcounts_take`, `mds_coverage`, `mds_coverage_take`, `mds_lastwriter`, `mds_lastwriter_get`, `mds_lastwriter_query` |
| idle only | `mds_load_rom` ("loading a ROM needs an idle console"), `mds_reset` ("reset needs an idle console"), `mds_savestate`, `mds_loadstate` ("savestates need a frame boundary; finish the frame first") |

The calls marked *debugger* above (`mds_interrupt` and the debugger calls) also need a console created
with `debug != 0`; on one created with `debug = 0` they fail with `console N was created without the
debugger (debug = 0)` (see [section 2](#creating-and-destroying)).

### Which calls block

- **Command-queue calls** (everything that reads or changes the console: `mds_read`, `mds_write`,
  `mds_screen`, `mds_get_regs`, the debugger calls, `mds_load_rom`, `mds_reset`, the savestate
  calls, `mds_mac`, the RTC calls, `mds_gdb`, and `mds_lua_run`/`mds_lua_exec`/`mds_lua_stop`)
  block until the emulator thread has run them: immediately when idle or parked, up to one frame
  when running. If the emulator thread is itself blocked (inside
  the GDB stub, or in a wireless receive waiting up to the MP timeout) they wait for that too
  *(inferred)*.
- **`mds_run`** blocks until every console it started has stopped, or the timeout passes.
  **`mds_wait`** blocks the same way without starting anything.
- **`mds_destroy`** joins the emulator thread: it waits for the current frame (or wait/park) to
  end.
- **Non-blocking:** `mds_start`, `mds_stop_after`, `mds_interrupt`, `mds_state`, `mds_frames`,
  `mds_take_break`, the input setters and the `mds_input_*` calls (they only lock the input queue),
  `mds_lua_output`, the packet calls, `mds_save_path`.
- The Lua calls run the script on the emulator thread, so they are serialised with every other
  caller's commands (the socket server's included); a script body runs there until its first
  `emu.frameadvance()` before the call returns. (API level 1 ran them on the calling thread.)

### Return values and errors

- Functions returning `int` return **-1 on failure** and set the error message. On success they
  return 0, or a count, an id, a queue depth, a length, as documented per function.
- `mds_run` and `mds_wait` return 0 (all stopped), 1 (timeout) or -1.
- `mds_take_break` returns 1 (an event was copied), 0 (none) or -1.
- Exceptions to the -1 rule: `mds_frames` returns 0 for an unknown id; the legacy
  `mds_lastwriter_get` returns 0 for every error (the same value as "no record"; use
  `mds_lastwriter_query`); `mds_has` returns 0 or 1; `mds_destroy`, `mds_serve_stop`,
  `mds_serve_name` and the setters `mds_set_local_dir`, `mds_set_log_level`, `mds_set_pacing`
  return nothing.
- Buffer sizes and counts (`max`, `max_packets`, `max_data`, `mds_run`/`mds_wait`'s `count`) must
  not be negative: a negative one fails with `max must not be negative` (or the parameter's name)
  before anything is copied. 0 is allowed and copies nothing.
- **`mds_error()`** returns the message of the most recent failure **on the calling thread**: it is
  a `thread_local std::string` in `Api.cpp`. Errors raised on the emulator thread are copied back
  to the caller's thread. A successful call does not clear it, so read it only right after a -1.
  The pointer stays valid until the next failure on the same thread.
- Messages are plain text. Input errors start with a protocol code (`bad_payload: ...`,
  `input_queue_full`) because the socket server maps them to error codes.

### Library-wide settings

| Function | Does |
| --- | --- |
| `int mds_version(void)` | The API level: 2 for this branch (`MDS_API_VERSION` in `melonds_api.h`), 1 for every library before it. See [Version and features](#version-and-features). |
| `int mds_has(const char* name)` | 1 if the library has the named feature or exports the named function, else 0 (also for NULL). Level 2 and later; on a level-1 library look the symbol up. |
| `void mds_set_local_dir(const char* dir)` | Directory for the files the melonDS core opens *by relative name* through `Platform::OpenLocalFile` (DSi NAND, SD/FAT image index, OpenGL shader cache). NULL means `.`. It does **not** affect BIOS, firmware, ROM, save or state paths: those are opened exactly as given, relative to the process's working directory. Process-wide, not synchronised: set it before creating consoles. |
| `void mds_set_log_level(int level)` | Core log threshold: 0 Debug, 1 Info, 2 Warn (default), 3 Error. Messages at or above it go to stderr. Process-wide. |
| `void mds_set_pacing(int fps)` | 0 (default, also any value <= 0): run as fast as possible. Otherwise each console thread sleeps so that it runs at most `fps` frames per second; a console that falls behind does not catch up (the schedule restarts from now). Process-wide, read every frame. |
| `int mds_mp_timeout(int ms)` | Receive timeout of the in-process wireless link, in ms (default 25). `ms >= 0` sets it; -1 only reads it. Returns the current value. Fails with "create a console first" until the first `mds_create` of the process. |

### Version and features

`mds_version()` is the API level. It goes up whenever functions are added or a behaviour changes;
`mds_has(name)` tells about one feature. Clients that must also run against older libraries should
look `mds_has` up as a symbol first (it does not exist at level 1), then ask it. The Python wrapper
does this: `native.api_version()`, `native.has(name)` (see [section 9](#9-python-equivalents)).

| level | branch | changes |
| --- | --- | --- |
| 1 | up to the IPC merge (`merge/ipc`) | everything in the C API before this table existed; `mds_version` returned 1 throughout, as functions were added |
| 2 | `fix/api-quirks` | `mds_has`, `mds_serve_host`, `mds_gdb_host`, `mds_lastwriter_query`; the behaviour changes named by the features below |

Features of level 2 (`mds_has` also answers 1 for every exported function name, `"mds_serve_host"`,
`"mds_read"`, ...):

| feature | meaning |
| --- | --- |
| `api_version` | `mds_version` reports the API level; `mds_has` exists |
| `debug_off_errors` | debugger calls fail on a console created with `debug = 0` ([section 2](#creating-and-destroying)) |
| `count_checks` | negative `max`/`count` arguments fail |
| `load_rom_idle` | `mds_load_rom` needs an idle console |
| `lua_queue` | the Lua calls run on the emulator thread; a first-step error fails `mds_lua_run` ([section 7](#7-lua)) |
| `step_exact` | `mds_step(n)` executes n instructions from idle and from parked ([Stepping](#stepping-mds_step)) |
| `dma_break` | DMA watchpoint breaks park at the DMA, `pc` = the DMA tag ([Breaks](#breaks-what-happens-and-mds_take_break)) |
| `lastwriter_query` | `mds_lastwriter_query` ([Last writer](#last-writer)) |
| `savestate_input` | savestates 14.2 carry `KeyInput`, the pen and the held input; a load restores them ([section 6](#6-savestates)) |
| `savestate_debug_reset` | a state load clears call stacks, a pending watchpoint hit and the last-writer records |
| `mic_noise_emulated` | level-mic noise is seeded from the emulated frame, which states carry |
| `serve_host` | `mds_serve_host`, `melonds_server --host` ([section 8](#8-gdb-stub-and-socket-server)) |
| `gdb_host` | `mds_gdb_host`; the GDB stub listens on 127.0.0.1 by default |

### Wireless between consoles, and one console per process

The first `mds_create` installs melonDS's *LocalMP* interface: every console in the process is on
one emulated local-wireless link (and one shared Slirp network for Wi-Fi). Consoles in different
processes are never linked. Local multiplayer therefore needs all consoles in one process, run
together (`mds_run` with several ids, or `mds_start` on each), and a blocking wireless receive
waits up to `mds_mp_timeout` ms for a partner.

For deterministic work (scenario replays, contrast pairs, coverage diffs) keep **one console per
process**. The dq9-re harness found that a second console in the same process changes a game's run
even while it is idle (different RAM hash for DQ9; see the dq9-re playbook, "Lessons"). The
mechanism is not pinned down in the code *(inferred: the shared LocalMP state and its receive
timeouts)*; the rule is empirical.

---

## 2. Lifecycle and running

### Creating and destroying

```c
int  mds_create(const char* bios9, const char* bios7, const char* firmware, const char* nickname, int debug);
void mds_destroy(int id);
```

- `bios9`, `bios7`: paths to BIOS dumps, or NULL/"" for melonDS's built-in FreeBIOS. A file must be
  exactly the BIOS size (4 KiB ARM9, 16 KiB ARM7) or creation fails with `bad BIOS image: PATH`.
- `firmware`: a firmware dump, or NULL/"" for generated firmware. Errors: `cannot open firmware`,
  `bad firmware image`.
- `nickname`: if not empty, the first 10 bytes become the firmware user nickname (each byte taken
  as one UTF-16 code unit).
- `debug`: nonzero builds the CPUs in the debug interpreter (`InterpreterGDB` mode), which calls the
  debugger hooks before every instruction and on every data access. **A console created with
  `debug = 0` has no debugger**: the hooks never run, so every call that would silently do nothing
  fails instead, with `console N was created without the debugger (debug = 0)`: `mds_bp_add`,
  `mds_wp_add`, `mds_point_remove`, `mds_point_enable`, `mds_point_list`, `mds_step`, `mds_trace`,
  `mds_trace_take`, `mds_calls`, `mds_callstack`, `mds_callcounts_take`, `mds_coverage`,
  `mds_coverage_take`, `mds_lastwriter`, `mds_lastwriter_query` (and `mds_lastwriter_get`, which
  returns 0), `mds_interrupt`, `mds_gdb`/`mds_gdb_host`, and in Lua the `event.onmemory*` hooks and
  the `debugger.*` functions except `peek` and `resume` (a Lua error with the same text). The socket
  server answers these with `not_supported`. Everything else (running, input, memory, registers,
  savestates, Lua without hooks, packet capture) works. Use `debug = 0` only for speed. The Python
  wrapper defaults to `debug=True`, as do the dq9-re harness and `melonds_server`.
- The new console is reset but **not started**: until `mds_load_rom` or `mds_reset`, running it
  emulates nothing (tests' `base.py`: "run() does nothing until the first reset()"). A console
  without a cart can be reset and run; FreeBIOS then spins in BIOS code.
- The firmware MAC is changed for effective ids above 0 (see [section 8](#8-gdb-stub-and-socket-server)).

`mds_destroy` stops the console (the frame in progress finishes), joins its thread, fails every
command still queued for it ("console is shutting down"), releases barrier partners waiting for it
and removes it from the wireless link. An unknown id is ignored. Do not destroy a console while
another thread is still calling into it: the registry lookup does not keep it alive.

Consoles that are never destroyed do not hang process exit (the registry is never destructed; test
`test_exit.py`), but closing them is cleaner.

### Loading a ROM and the save file

```c
int mds_load_rom(int id, const char* rom, const char* save);
int mds_save_path(int id, char* out, int max);
```

`mds_load_rom` needs an **idle** console, like `mds_reset` (a parked one fails with "loading a ROM
needs an idle console"). It reads the ROM, resolves the save path, inserts the cart and resets the
console (errors: `cannot read ROM: P`, `cannot parse ROM: P`).

- `save` NULL or "" selects the default: the ROM path with its extension replaced by `.sav` for
  effective id 0, and `.sav.N+1` for effective id N > 0 (`game.sav`, `game.sav.2`, ...; test
  `test_save_numbering`).
- If the save file exists, its contents become the cart's save memory. If not, the cart starts
  with blank save memory and the file is created on the first write.
- **The save file is live.** Every completed save write by the game (each SPI write sequence, on
  chip-select release) rewrites the whole file at once (`Platform::WriteNDSSave` ->
  `Instance::WriteSave`). **`mds_loadstate` also rewrites it** with the save memory stored in the
  state (`CartRetail::DoSavestate`). That is deliberate and kept for now (see
  [section 6](#6-savestates)): the cart's save memory is part of the state, and the file follows
  it. It matters because a save booted in place is overwritten by the first state load (with the
  save memory of whatever run made the state). **Boot a copy**: give every console its own copy
  and never point it at a save you want to keep (the dq9-re harness copies every save into a temp
  folder before booting it).
- Boot: with FreeBIOS or generated firmware the cart is direct-booted. With real BIOS and a
  bootable firmware dump, melonDS's `NeedsDirectBoot` is false and the console boots through the
  firmware *(inferred)*.
- Breakpoints, watchpoints, coverage and call counts survive a ROM load (they are not part of the
  reset; see the table in [section 5](#5-debugging)).

`mds_save_path` copies the path the last `mds_load_rom` resolved to (with its NUL) and returns its
length; "" (length 0) before any ROM was loaded. Fails with "buffer too small" if `max <= length`.

### Reset and the clock

```c
int mds_reset(int id);
int mds_set_rtc(int id, int year, int month, int day, int hour, int minute, int second);
int mds_get_rtc(int id, int* out);   /* out[6]: year, month, day, hour, minute, second */
```

`mds_reset` needs an **idle** console (parked is refused). It power-cycles with the same cart and
the save memory currently in RAM (the save file is not reread), direct-boots if needed and starts
the console. It also sets the frame counter to 0, empties the input queue, the input log and the
held input state, resets the touch timeline, empties the microphone FIFO and drops Lua's pending
input. Debugger effects are listed in [section 5](#5-debugging).

`mds_set_rtc` takes a full year 2000-2099 (else "RTC year must be 2000-2099"); other fields out of
range are clamped by the core (month 1-12, day valid for the month, else 1; hour/minute/second
else 0). The clock is emulated: it advances with emulated time, not wall time. It is not seeded
from the host: a new console reads 2000-01-01 00:00:00. A reset keeps the clock; a savestate
carries its own clock, so **set the clock after loading a state**, not before. Games that read the
clock only replay identically with the same clock.

### Running

```c
enum { MDS_IDLE = 0, MDS_RUNNING = 1, MDS_PARKED = 2 };

int      mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states);
int      mds_wait(const int* ids, int count, int timeout_ms, int* states);
int      mds_start(int id, uint32_t frames);
int      mds_stop_after(int id, uint32_t frames);
int      mds_interrupt(int id);
int      mds_state(int id);
int      mds_take_break(int id, mds_break* out);
uint64_t mds_frames(int id);
```

**States.** *Idle*: at a frame boundary, nothing running. *Running*: the emulator thread is running
frames (or is blocked at a frame boundary waiting for input or a barrier; `mds_state` still says
running, `mds_input_waiting` says waiting). *Parked*: stopped **inside** a frame at a debugger
break; the CPU is between two instructions.

**`mds_start(id, frames)`** starts a stopped console for `frames` frames and returns at once. There
is no "forever": `frames` is a budget of frame ends, and the Python wrapper's default 0xFFFFFFFF is
just very long. From idle, `frames = 0` stops again at once. From **parked** it resumes: the frame
that was interrupted finishes and counts as one of the `frames`, and the remaining budget from
before the break is discarded. `mds_start(id, 0)` and `mds_start(id, 1)` on a parked console both
finish the current frame and go idle *(verified for 0)*. Starting a running console fails
("console N is running"). Starting clears the pending break event, so call `mds_take_break` first.

**`mds_run(ids, count, frames, timeout_ms, states)`** checks that every console is stopped (if one
is running it fails and starts none), starts each with `frames`, then behaves as `mds_wait`.

**`mds_wait(ids, count, timeout_ms, states)`** waits until none of the consoles is running.
`timeout_ms < 0` waits forever; 0 polls once. If one console parks while others are still running,
`mds_wait` interrupts the running ones (once per call), so a break in one console stops the group;
those consoles park too (break kind `request`). Returns 0 when none is running, 1 on timeout (the
consoles **keep running**: stop them with `mds_stop_after` and wait again), -1 for an unknown id.
`states`, if not NULL, receives each console's `MDS_*` state.

**`mds_stop_after(id, frames)`** changes the remaining budget of a *running* console (ignored when
idle or parked). The frame in progress always finishes; `frames` counts frame ends from now
including that one, so 0 and 1 both stop at the end of the current frame. One difference: a
console blocked waiting for input stops without running a frame on 0, and runs one more frame
(when input arrives) on 1.

**`mds_interrupt(id)`** (needs the debugger) asks the console to park as soon as possible. It sets a break request on
**both** CPUs; the first CPU to execute an instruction parks with kind `request`. A console blocked
waiting for input (or at a barrier) parks at the frame boundary instead, without running a frame
(test `test_interrupt_parks_at_the_frame_boundary`). Caveats *(verified)*: the other CPU's request
stays armed, so the next resume parks again almost at once with a second `request` break on the
other CPU; and an interrupt sent to an idle console parks the next run at its first instruction.
There is no call to cancel a request. When a frame boundary is good enough, prefer
`mds_stop_after(id, 0)`.

**`mds_state(id)`** returns `MDS_IDLE`, `MDS_RUNNING`, `MDS_PARKED` or -1.

**`mds_frames(id)`** returns the number of frames completed since the last reset or ROM load (0
for an unknown id). It is incremented at the end of each frame, so a parked console has not yet
counted the frame it is in. It is **not** part of savestates: loading a state leaves it alone
(test `test_save_modify_load_restores`). It counts the process's frames; the console's emulated
frame count (which seeds the level-mic noise) is a separate counter that savestates 14.2 carry.

### Breaks: what happens and `mds_take_break`

When a breakpoint, watchpoint, step, request or fault fires, the emulator thread parks inside the
CPU loop: state becomes `MDS_PARKED`, the break is recorded, and the thread serves commands until
resumed. Registers, memory, the call stack and the trace can be read while parked; points can be
added and removed. Resume with `mds_start` or `mds_run`.

```c
typedef struct {
    int32_t  kind;   /* 0 exec, 1 read, 2 write, 3 step, 4 request, 5 fault */
    int32_t  cpu;    /* 9 or 7 */
    int32_t  size;   /* access size 1/2/4 for read/write; 0 otherwise */
    int32_t  id;     /* point id for exec/read/write; 0 for step, request, fault */
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    int32_t  dma;    /* DMA channel 0-3 for a DMA write, else -1 */
} mds_break;
```

| kind | `pc` | `addr` | `value` | the CPU stopped... |
| --- | --- | --- | --- | --- |
| exec | the breakpoint address (instruction not executed yet) | = pc | 0 | before the instruction |
| read / write | the instruction that made the access | the accessed address (start of the access) | write: the value written; read: memory at `addr` read through the debug path | before the **next** instruction of that CPU (the access has completed) |
| write by DMA (`dma >= 0`) | the DMA tag `0xFFFFFFF0 \| channel << 1 \| arm7` (no instruction made the write) | the unit's destination | the unit written | **at once**, inside the DMA: the unit is written, the next one is not (the CPUs are between two instructions) |
| step | the next instruction | = pc | 0 | before that instruction |
| request | the next instruction; for a request served at a frame boundary (waiting for input) it is the raw ARM9 R15, which is one pipeline stage ahead | = pc | 0 | before that instruction |
| fault | the faulting instruction (ARM9 MPU prefetch or data abort) | the data address, or pc | 0 | before the abort is taken |

For a DMA break `cpu` is the CPU whose DMA controller made the write (9 for channels of the ARM9's
DMA, 7 for the ARM7's), `dma` the channel 0-3 and `size` the unit size (2 or 4). The tag is the
same encoding as the last-writer records, so `pc >= 0xFFFFFFF0` always means "a DMA, not an
instruction". Resuming finishes the DMA (each further watched unit breaks again) and then the CPU
continues. While parked there, `mds_get_regs` index 15 of that CPU is its last executed instruction,
which did not make the write (use `r15_raw`). The program that started the DMA is not recorded.
(API level 1 queued DMA hits for the CPU's next instruction and reported that CPU's last
instruction as `pc`.)

`mds_take_break(id, out)` copies the **latest** break and clears it; it returns 0 if there is none.
Only one event is kept: if the console parked twice without a take, the first is lost, and starting
the console clears it. If several points match at once, the first one wins and the others only
count a hit. While a socket client is connected, the server thread takes the break events itself
(they go to the client), so in-process callers see 0.

### Stepping: `mds_step`

```c
int mds_step(int id, int cpu, uint32_t count);   /* cpu: 7 or 1 = ARM7, anything else = ARM9 */
```

`mds_step` does not run anything. It **arms** a countdown on one CPU; the next `mds_start` /
`mds_run` runs the console and parks it with a `step` break when the countdown reaches zero.
`mds_step(id, cpu, n)` then a run executes exactly `n` instructions of that CPU and parks before the
next one, from **idle** and from **parked** alike (tests `StepExact` in `test_api_fixes.py`):
`mds_step(id, 9, 1)` from idle executes one ARM9 instruction, then parks; parked at a breakpoint,
the instruction at the breakpoint is the first of the `n`. The same holds for the socket server's
`step` message and Lua's `debugger.step`, which use the same countdown. (API level 1 executed `n - 1`
from idle, and from any park that was not in that CPU's own instruction hook.)

`count = 0` disarms. The other CPU keeps running while one is stepped; the whole console parks. If
the CPU is halted (waiting for an interrupt) the step completes when it wakes. Give the run a frame
budget large enough to reach the step (the socket server uses a cap); the frame count advances only
when a frame ends. The step break is reported through `mds_take_break` with kind 3.

---

## 3. Input

Every input source feeds one per-console FIFO of **payloads**, one payload per frame:
`mds_set_keys` / `mds_touch` / `mds_set_lid`, `mds_input_submit`, `mds_input_submit_multi`, Lua's
`joypad.set` / `input.NDSTapDown`, and the socket server. At the start of each frame the emulator
thread takes the front payload, or applies the empty-queue rule, then runs the frame.

### Keys

Button bits (12 bits; bits above 11 are an error in payloads and are masked off by `mds_set_keys`):

| bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| key | A | B | Select | Start | Right | Left | Up | Down | R | L | X | Y |

A set bit means **pressed**. Keys are written to the hardware every frame (`ApplyButtons`).
Savestates 14.2 carry `KeyInput` and the held input too (see [section 6](#6-savestates)).

### The setters

```c
int mds_set_keys(int id, uint32_t pressed);
int mds_touch(int id, int x, int y);      /* x or y < 0: pen up; clamped to 255 / 191 */
int mds_set_lid(int id, int closed);
```

They are conveniences over the queue (`InputQueue::Modify`): each builds a payload from the
*newest* state (the last queued payload, else the last applied one), changes one thing, and either
**amends** the newest queued payload if that is also a setter payload, or appends a new one. So:

- they apply at the next frame start, not immediately (test
  `test_setters_apply_at_the_next_frame_not_immediately`);
- keys and touch set before one frame land in the same frame (`test_keys_and_touch_before_a_frame_land_together`);
- each setter keeps the other state (setting keys does not lift the pen);
- a setter goes **behind** payloads already queued (`test_setter_goes_behind_queued_payloads`);
- setter payloads always carry one touch point at offset 0 (the pen state).

What happens after the setter payload is consumed depends on the empty-queue mode: in the default
repeat mode the state is held until changed (local sources never stall), in neutral mode buttons
and pen are released the next frame, in wait mode the console waits for the next payload.

### Queued payloads

```c
typedef struct { uint32_t offset; uint16_t x, y; uint8_t down; uint8_t pad[3]; } mds_touch_point;

typedef struct {
    uint32_t seq;          /* free for the caller; logged when flags bit 0 is set */
    uint16_t buttons;      /* 12 bits, pressed = 1 */
    uint8_t  lid;          /* 1 = closed */
    uint8_t  touch_count;  /* 0 = pen unchanged */
    uint8_t  mic_kind;     /* 0 none, 1 samples, 2 level */
    uint8_t  flags;        /* bit 0: treat as a socket client's payload */
    uint16_t mic_count;    /* samples in mic[] for kind 1 */
    uint16_t mic_level;    /* kind 2: noise amplitude 0-65535 */
    uint16_t pad2;
} mds_input;

int mds_input_submit(int id, const mds_input* in, const mds_touch_point* points, const int16_t* mic);
```

Returns the new queue depth, or -1 with `bad_payload: ...` or `input_queue_full`; nothing is queued
on failure. Validation (`InputQueue::Validate`): at most 16 touch points, offsets strictly
increasing and below 560190, x <= 255, y <= 191; at most 796 mic samples; `mic_kind <= 2`; no
button bits above 11; `points` / `mic` may only be NULL when their counts are 0. The queue holds
600 payloads (10 s at 60 fps).

- **Touch** (`touch_count > 0`): the points are a *sub-frame timeline*, see below. With
  `touch_count = 0` the pen stays as it was.
- **Mic**: kind 1 pushes `mic_count` samples; kind 2 generates noise of amplitude `mic_level`;
  kind 0 pushes nothing. See the mic FIFO below.
- **`flags` bit 0** marks the payload as coming from a socket client: its `seq` is then kept in the
  input log, and the repeat-mode stall timeout applies to it. Other bits are ignored.
- **`seq`** has no effect on emulation. In-process, it is only visible in the input log (and only
  for flag-bit-0 payloads; local payloads log 0). Over the socket, the frame event that applies a
  payload echoes its seq (`FrameEvent::AppliedSeq`, `Protocol.h`).

### Empty-queue modes and queue control

```c
int mds_input_mode(int id, int mode, int stall_frames);  /* mode: 0 repeat, 1 neutral, 2 wait, -1 unchanged;
                                                            stall_frames: -1 unchanged, 0 never */
int mds_input_depth(int id);     /* payloads queued */
int mds_input_waiting(int id);   /* 1 while blocked at a frame boundary for input or a barrier */
int mds_input_clear(int id);     /* drop queued payloads */
```

What a frame gets when the queue is empty (`InputQueue::Next`):

- **repeat** (default): the last applied buttons, lid and pen. If the last consumed payload came
  from a socket client (flag bit 0) and `stall_frames > 0` (default 15), after `stall_frames` empty
  frames buttons and pen are released (the lid stays): a client that disappears cannot leave a key
  held (tests `test_client_payload_stalls_in_repeat_mode`, `test_local_payload_never_stalls`).
- **neutral**: buttons and pen released on every empty frame (lid kept).
- **wait**: the emulator thread blocks at the frame boundary without consuming frame budget until a
  payload arrives, the mode changes, the console is stopped (`mds_stop_after`) or interrupted
  (parks with `request`), or a command arrives (served, then it keeps waiting). Test class
  `WaitMode` in `test_input.py`.

Mic data is never repeated. A mode change applies from the next frame. The mode and stall timeout
survive reset and savestate loads. **`mds_serve` and every socket client connect/disconnect reset
the mode of every console to repeat with stall 15** (see [section 8](#8-gdb-stub-and-socket-server)).

`mds_input_clear` drops the queued payloads (leaving their barrier groups) and keeps the held state
and the log. `mds_reset` / `mds_load_rom` also clear the log and the held state. `mds_loadstate`
clears the queue and, for a 14.2 state, **restores the held state** the state was saved with (the
last applied buttons, lid and pen, whether it came from a socket client, and the empty frames since,
so the stall countdown continues): after a load the repeat rule repeats the loaded state's input,
not what was held before the load. A 14.2 state saved by another frontend (no headless section)
takes the held buttons, lid and pen from the loaded `KeyInput` and touch controller. A 14.1 or
older state leaves the held state as it was before the load (the level-1 behaviour). The mode, the
stall timeout and the log are never touched by a load.

### The input log

```c
typedef struct {
    uint64_t frame;        /* mds_frames() value of the frame the payload was applied to */
    uint32_t seq;          /* 0 for in-process sources */
    uint16_t buttons;
    uint8_t  lid;
    uint8_t  touch_count;
    uint8_t  mic_kind;
    uint8_t  pad;
    uint16_t mic_level;
    uint16_t pad2, pad3;
} mds_input_log_entry;

int mds_input_log(int id, uint64_t from_frame, mds_input_log_entry* out, int max);
```

Copies up to `max` entries with `frame >= from_frame`, oldest first, and returns how many. Only
**consumed payloads** are logged, one entry per payload: frames that ran on the empty-queue rule
(repeat/neutral) have no entry, so gaps in `frame` are normal. The log keeps the last 216,000
entries (an hour at 60 fps) and at most 32 MiB of touch/mic data. Reading does not clear it.
`frame` restarts at 0 after a reset and does not jump on a state load. In-process `seq` is 0 unless
the payload had flag bit 0. `touch_count` is the number of points actually applied, which is 1 for
a payload without points (the held pen), never 0. The touch points and mic samples are not exposed
by the C API (the socket's `input_log_read` returns full payloads).

### Barrier groups: `mds_input_submit_multi`

```c
int mds_input_submit_multi(const int* ids, int count, const mds_input* inputs,
                           const mds_touch_point* const* points, const int16_t* const* mic, int* depths);
```

Queues one payload for each console as one **barrier group**: all or none are queued (unknown
console, a bad payload, a console listed twice, or any full queue fails the whole call). When a
console reaches its group's payload at a frame boundary it waits (`mds_input_waiting` = 1) until
every member has reached its own payload, then all apply them and start the frame. `points[i]` and
`mic[i]` belong to `ids[i]`; the arrays or individual entries may be NULL when the counts are 0.
`depths[i]` receives each queue's depth. Returns 0 or -1.

A waiting member is released early when another member can no longer arrive: it was destroyed, its
queue was cleared or it was reset, or **it stopped running** (its frame budget ran out). A member
that stops is dropped from every group it belongs to, including groups whose payloads are still
queued, so later barriers do not wait for it either (test
`test_stopped_member_with_payload_left_does_not_block_later`). Barriers therefore only synchronise
consoles that run concurrently: start them with one long `mds_run`/`mds_start`, not one frame at a
time. A member is also released while a partner is blocked in a wireless receive (a parked console
cannot answer that partner's packets). A waiting console still serves commands, stops, and parks on
`mds_interrupt` (tests in `test_barrier.py`).

`mds_debug_recv_pending(id, pending)` is a test hook: it marks the console as blocked in (1) or out
of (0) a wireless receive, as the platform layer does around a blocking MP receive. Calls nest.

### Sub-frame touch timeline

Touch points carry `offset`, in system cycles (33.5 MHz) from the start of the frame; a frame is
560190 cycles, 263 scanlines of 2130 cycles, so scanline `n` starts at `n * 2130`. Each point holds
from its offset until the next point. Before the first point the pen keeps the state the previous
frame ended in; the last point carries into later frames until another payload has points. The core
consults the timeline whenever the ARM7 samples the touch controller, so a payload can press,
move and release within one frame (tests `test_touch.py`, `TouchTimelineTests.cpp`). The state at
offset 0 is also applied directly at frame start. A single point at offset 0 is the same as
`mds_touch` (`test_mds_touch_matches_a_single_offset_zero_payload`).

### Microphone FIFO

Each console has a sample FIFO (`MicFifo`) that the core reads at the touch controller's mic rate
(one sample per 704 system cycles, 795 or 796 per frame). It holds four frames (3184 samples) and
drops the oldest beyond that. When it is empty the core holds the last sample. Samples are only
accepted while the game has the microphone on (`Mic_Start`); samples sent while it is off are
dropped (`test_samples_sent_while_the_mic_is_off_never_arrive`), and turning it on or off empties
the FIFO. Kind 2 (`mic_level`) generates `795 or 796` samples of xorshift noise uniform in
`[-level/2, level/2]`, seeded from the console's **emulated frame** (frames since the last reset or
ROM load, `Instance::EmuFrame`), so level 0 is silence and the same game frame gives the same noise.
Without loads the emulated frame equals `mds_frames`; unlike it, savestates 14.2 carry it. A reset
empties the FIFO. A load of a 14.2 state puts back the samples the FIFO and the core's own mic
buffer held when the state was saved, so the same state and inputs give the same mic samples with or
without a load in between (tests `MicAcrossLoad`); a 14.1 or older state empties both and keeps the
emulated frame count from before the load (the level-1 behaviour, where the seed was `mds_frames`).

### Which applies when sources are mixed

There is one FIFO and one payload per frame. Payloads apply in the order they were queued, with
one exception: a setter call amends the newest payload when that payload is itself an unconsumed
setter payload. Lua's input is gathered while a script runs and submitted as one normal payload
when the script yields (or `mds_lua_exec` returns), behind whatever is queued (test
`test_joypad_set_lands_in_the_queue_after_queued_payloads`). When the queue is empty the mode
decides.

---

## 4. Memory and CPU

### Reading and writing memory

```c
int mds_read(int id, int cpu, uint32_t addr, uint32_t len, uint8_t* out);
int mds_write(int id, int cpu, uint32_t addr, uint32_t len, const uint8_t* data);
```

`cpu` 7 or 1 selects the **ARM7 bus**; any other value (9, 0, ...) the **ARM9 bus**. Both run on
the emulator thread (at the next frame boundary when running) and work in any state.

`mds_read` copies `len` bytes as that CPU sees them:

- ARM9: ITCM (below the ITCM size) and DTCM (at the DTCM base) come from the TCM arrays, so a DTCM
  mapped over main RAM is seen as the CPU sees it.
- Main RAM (`0x02xxxxxx`, mirrored) is read directly.
- **I/O (`0x04xxxxxx`) reads as 0**: registers are not read, so reads have no side effects and
  cannot show register values.
- Everything else goes through the core's bus read (`NDS::ARM9Read8` / `ARM7Read8`): shared and
  ARM7 WRAM, palette, VRAM (as currently mapped), OAM, GBA slot, BIOS. The ARM7 BIOS
  (`< 0x4000`) reads as 0xFF unless the ARM7 PC is inside the BIOS (the core's BIOS protection).

`mds_write` writes byte by byte through `Debugger::Poke`:

- TCM and main RAM writes work; code written to RAM runs correctly (interpreter, no JIT cache).
- **I/O writes are refused**: the call fails with "address not writable" at the first I/O byte,
  after writing the bytes before it.
- **ARM9 byte writes to palette, VRAM and OAM (`0x05`-`0x07`) and to the GBA ROM area are dropped by
  the bus (as on hardware), but the call still returns 0** *(verified for palette)*. There is no
  16- or 32-bit write in the C API.
- Writes do not trigger watchpoints and are not recorded by the last-writer table *(inferred: they
  bypass the CPU access hooks)*.

### Registers

```c
int mds_get_regs(int id, int cpu, uint32_t* out);   /* out[19]; console stopped */
int mds_set_reg(int id, int cpu, int reg, uint32_t value);
```

`out` layout:

| index | content |
| --- | --- |
| 0-14 | r0-r14 as seen in the CPU's **current mode** (melonDS keeps the active bank in `R[]`; other modes' banked r8-r14 are not exposed) |
| 15 | pc: the address of the next instruction to execute. Idle: `R15 - 4` (ARM) or `- 2` (Thumb). Parked: the debugger's last-seen pc of that CPU (see the caveat) |
| 16 | CPSR |
| 17 | SPSR of the current mode, 0 in User/System |
| 18 | raw R15 (between instructions: pc + 4 in ARM, pc + 2 in Thumb) |

Caveat *(verified)*: while parked, index 15 is `Debugger::CurPC` of the requested CPU. That is
exact for the CPU that broke at an instruction (not for a DMA break, where it is that CPU's last
executed instruction). For the other CPU it is the last instruction it executed, and if no
debugger feature armed that CPU (for example only an ARM9 breakpoint exists) it is stale, 0 after a
reset. For the other CPU use `out[18] - 4` (ARM) or `- 2` (Thumb), with the Thumb bit from CPSR.

`mds_set_reg` sets r0-r15 of the current mode (`register out of range` otherwise); CPSR and SPSR
cannot be set. `reg = 15` jumps (flushing the pipeline) to `value` and keeps the current ARM/Thumb
state: in Thumb state the address is forced odd, and an odd address in ARM state switches to Thumb.
Setting a register while parked at a breakpoint affects the instruction about to execute
*(inferred)*.

### Screen

```c
int mds_screen(int id, uint32_t* out);   /* 256 * 384 words */
```

Copies both screens of the last **completed** frame: the top screen (256x192) first, then the
bottom screen, as the player sees them (the core's screen swap is applied). Each pixel is a 32-bit
word `0xAARRGGBB` with alpha 0xFF, i.e. bytes B, G, R, A in memory (BGRA; PIL:
`Image.frombuffer("RGBA", (256, 384), buf, "raw", "BGRA", 0, 1)`). The 6-bit DS colours are
expanded to 8 bits. The frame buffer is swapped at the end of each frame, so after `mds_run(1)` the
screen shows that frame. Framebuffers are not in savestates: right after `mds_loadstate` the screen
still shows the frame from before the load, and 3D content lags a frame more; run two frames after
a load before trusting it (dq9-re harness experience). Works in any state; a running console
answers at the next frame boundary. Fails with "no framebuffer" if the renderer has none.

---

## 5. Debugging

Everything in this section except packet capture needs a console created with `debug != 0`; on
one created with `debug = 0` the calls fail with `console N was created without the debugger
(debug = 0)` (see [section 2](#creating-and-destroying)). Setup calls need a stopped console. `cpumask` arguments
use bit 0 = ARM9 and bit 1 = ARM7 (3 = both); `cpu` arguments use 7 or 1 for ARM7, anything else
for ARM9.

### Breakpoints and watchpoints

```c
int mds_bp_add(int id, uint32_t cpumask, uint32_t addr);
int mds_wp_add(int id, uint32_t cpumask, uint32_t addr, uint32_t size, uint32_t flags);  /* flags: 1 read, 2 write */
int mds_point_remove(int id, int point);
int mds_point_enable(int id, int point, int enabled);
int mds_point_list(int id, mds_point* out, int max);

typedef struct {
    int32_t  id;
    int32_t  watch;     /* 0 breakpoint, 1 watchpoint */
    uint32_t cpumask;
    uint32_t flags;     /* watchpoints: 1 read, 2 write; breakpoints: 0 */
    uint32_t enabled;
    uint32_t start;
    uint32_t end;       /* inclusive; = start for breakpoints */
    uint64_t hits;
} mds_point;
```

- `mds_bp_add` returns the new point id. Bit 0 of `addr` is cleared, so a Thumb function address
  with its interworking bit works. The CPU parks before executing the instruction at that address.
- `mds_wp_add` watches `[addr, addr + size - 1]` (clamped at 0xFFFFFFFF) for reads (flag 1),
  writes (2) or both (3); only `flags & 3` is used. `size = 0` or no read/write flag fails with
  "bad watchpoint". An access matches if it overlaps the range. It covers CPU loads and stores
  (including TCM) and, for writes, **DMA**: an ARM9 DMA channel write checks the ARM9 watchpoints
  (`cpumask` bit 0), an ARM7 DMA write the ARM7 ones. DMA source reads and other bus masters are not
  watched. A CPU access breaks before the next instruction of that CPU; a DMA write breaks at once,
  between two units, with `pc` = the DMA tag (see the break table in
  [section 2](#breaks-what-happens-and-mds_take_break)). Debugger reads and writes
  (`mds_read`/`mds_write`) do not trigger watchpoints.
- Ids come from one per-console counter starting at 1, shared by breakpoints, watchpoints and Lua
  memory hooks; they are never reused. `mds_point_remove` / `mds_point_enable` fail with "no such
  point".
- `mds_point_list` copies up to `max` points (`max >= 0`), all breakpoints first then all
  watchpoints, each in creation order, and returns the count copied (there is no total, so
  `n == max` may mean truncation). Lua memory hooks appear here too.
- `hits` counts every enabled match, including matches that did not park because another event
  was already pending or a Lua hook returned false. Disabled points are not matched.

Cost: with at least one enabled breakpoint, every instruction of that CPU calls the debugger hook
and checks a 1 MiB page table; with a watchpoint, every data access of that CPU does. Points
persist across reset, ROM loads and savestate loads.

### Instruction trace

```c
int mds_trace(int id, uint32_t cpumask, uint32_t start, uint32_t end, uint32_t capacity);
int mds_trace_take(int id, mds_trace_entry* out, int max);

typedef struct {
    uint64_t cycle;  /* ARM7: ARM7 timestamp; ARM9: ARM9 timestamp >> clock shift (system cycles) */
    uint32_t pc;
    uint32_t instr;  /* the opcode fetched for pc */
    uint32_t cpsr;
    uint32_t cpu;    /* 9 or 7 */
} mds_trace_entry;
```

`mds_trace` (re)configures a ring buffer of `capacity` entries recording every instruction of the
masked CPUs whose pc is in `[start, end]` (inclusive). `capacity = 0` turns tracing off. Each call
discards everything recorded and staged. When the ring is full the oldest entries are overwritten.
An instruction is recorded just before it executes, so the instruction a break stops at is in the
trace although it has not run yet (test `TraceTests`). Entries of both CPUs are stored in
emulation order, which is not strictly cycle order across CPUs *(inferred: melonDS runs the CPUs
in alternating batches)*.

`mds_trace_take` returns up to `max` entries, oldest first, and removes them. It drains the whole
ring into a staging buffer on the first call; a call that stops at `max` leaves the rest staged for
the next call, so loop until it returns 0. Cost: one hook call per instruction of the traced CPUs,
24 bytes per entry. A reset empties the ring (not the staged part); a savestate load does not.

### Call tracking: call stacks and call counts

```c
int mds_calls(int id, int enabled);
int mds_callstack(int id, int cpu, mds_frame* out, int max);
int mds_callcounts_take(int id, int cpu, uint32_t* entries, uint64_t* counts, int max);

typedef struct {
    uint32_t site;       /* address of the call instruction */
    uint32_t entry;      /* call target (Thumb bit cleared) */
    uint32_t ret;        /* return address (LR, bit 0 cleared) */
    uint32_t sp;         /* SP right after the call */
    uint32_t mode;       /* CPSR mode bits at the call (0x10 usr, 0x12 irq, 0x13 svc, 0x1F sys, ...) */
    uint32_t exception;  /* 1: an exception entry, not a call */
} mds_frame;
```

`mds_calls(id, 1)` turns tracking on for both CPUs; `mds_calls(id, 0)` turns it off and **clears the
call stacks** (not the counts).

**What counts as a call:** `BL`, `BLX` (immediate and register) in ARM state, and Thumb `BL`/`BLX`
(the second half of the pair) and `BLX Rm`. Nothing else: `B`/`BX` tail calls, `MOV PC, ...`,
`LDR PC, ...` and the `MOV LR, PC; BX Rn` idiom are not counted and do not push a frame. A call
through a veneer (`BL veneer; veneer: LDR PC/BX`) is counted at the veneer *(inferred)*. IRQs and
SWIs are tracked as exception frames but not counted.

**Call stack.** Each call pushes a frame (at most 4096 deep; the oldest frame is dropped beyond
that). Before every instruction the tracker pops frames whose return address has been reached in
the same mode with the stack back at or above the frame's SP, and frames whose SP the stack has
risen past (so tail calls and longjmp-style exits unwind). It is **thread aware** *(heuristic)*:
when SP jumps by 0x400 or more, or changes in the same mode without a stack-adjusting instruction,
the current stack is parked under the old SP and the parked stack whose SP is closest to the new SP
(within 0x200) is restored, keeping up to 64 parked thread stacks. It is **interrupt aware**: an
IRQ or SWI saves the interrupted stack (up to 16 nested) and starts a new one whose first frame is
the exception entry (`exception = 1`, `site = ret` = the interrupted address, `entry` = the vector
address); the exception return restores it. Unwinding only runs in debug mode.

`mds_callstack` copies the stack of one CPU **innermost first**: the current frames, then for each
interrupted context its exception frame and the frames below it. It returns the count copied
(empty if tracking is off). Frames from before tracking was enabled are unknown, so enable it
before running to the point of interest. A reset and a savestate load clear the stacks (a load
puts the CPUs somewhere else entirely; tracking stays on and builds new stacks from there).

**Call counts.** Each call increments a counter for its `entry`, per CPU.
`mds_callcounts_take` returns up to `max` (entry, count) pairs in no particular order and removes
them (staged like the trace: loop until 0). Counts accumulate across frames, resets and state
loads until taken; turning tracking off does not clear them. Python sums the chunks into a dict.

Cost: one hook call per instruction (unwinding) plus work per call; it also arms instruction hooks
for both CPUs.

### Coverage

```c
int mds_coverage(int id, int enabled);
int mds_coverage_take(int id, int cpu, uint32_t* out, int max);
```

Coverage records **every executed instruction address** (one bit per halfword, so ARM and Thumb
both resolve to exact instructions), per CPU. It is per instruction, not per block or function.
`mds_coverage_take` returns up to `max` addresses, sorted ascending and unique, and removes them
(staged: loop until 0). Turning coverage off stops recording but keeps what was recorded until
taken; reset and state loads keep it too.

Coverage catches code that call counts miss: code reached by `B`/`BX` (tail calls, jump tables,
switch cases, branches inside a function), code entered by `MOV PC`/`LDR PC`, interrupt handlers,
and which parts of a function ran. Call counts tell how often; coverage tells whether. Cost: a hook
call per instruction plus a hash lookup when the 4 KiB page changes; 256 bytes per touched page.

### Last writer

```c
int      mds_lastwriter(int id, int enabled);
uint32_t mds_lastwriter_get(int id, uint32_t addr);
```

`mds_lastwriter(id, 1)` allocates a 16 MiB table (one word per main-RAM word) and clears it;
`mds_lastwriter(id, 0)` frees it. While enabled, every write to **main RAM** (`0x02xxxxxx`, the
4 MiB mirror) records, for the 32-bit word containing it (so a byte write tags the whole word):

- a CPU store: the storing instruction's address with bit 0 set for ARM7 (`pc | 1`), clear for
  ARM9;
- a DMA write: `0xFFFFFFF0 | channel << 1 | cpu`, cpu bit 1 = ARM7.

```c
int mds_lastwriter_query(int id, uint32_t addr, uint32_t* raw, int* dma);
```

`mds_lastwriter_query` returns **1** with the record in `*raw` and the DMA channel in `*dma` (-1 for
an instruction), **0** for no record (never written since tracking was enabled, a reset or a state
load, or an address outside main RAM; `*raw = 0`, `*dma = -1`), or **-1** for an error: unknown
console, running console, no debugger, or "last-writer tracking is off (mds_lastwriter(id, 1))".
Either pointer may be NULL. Decode `raw`: `w >= 0xFFFFFFF0` DMA channel `(w >> 1) & 3` of CPU
`w & 1 ? 7 : 9` (the same tag a DMA watchpoint break reports as `pc`); else pc `w & ~1` of CPU
`w & 1 ? 7 : 9`. Tests: `WriterTests` (ARM9, ARM7, DMA), `LastWriterQuery`.

`mds_lastwriter_get` is the **legacy** form: it returns the raw record, and 0 both for "no record"
and for every error, which cannot be told apart. Lua's `debugger.lastwriter(addr)` returns
`pc, cpu` for an instruction, `nil, cpu, channel` for a DMA write and `nil` for no record.

A reset and a state load zero the table (the records described the RAM before the load). Cost: a
hook on every data access of both CPUs (it arms the data hooks) and 16 MiB.

### Wireless packet capture

```c
int mds_packets(int id, int enabled, int capacity);
int mds_packets_take(int id, mds_packet* headers, uint8_t* data, int max_packets, int max_data);

typedef struct { uint64_t timestamp; uint32_t sent; uint32_t type; uint32_t length; } mds_packet;
```

`mds_packets` turns capture on or off for one console and empties the ring and the staging; the
ring holds `max(capacity, 1)` packets and drops the oldest. Logged: sent packets (`type` 0 data, 1
host command, 2 reply, 3 ack) and received packets (`type` 0 data, 1 host packet); replies received
by a host are not logged. `timestamp` is the core's wireless timestamp. It works in any state and
also when debug is off.

`mds_packets_take` copies headers and packs the packet bytes back to back into `data`; it stops
before the first packet that does not fit in `max_data` (see Known issues) and returns the number of
packets copied. Staged like the others: loop until 0. `max_packets` and `max_data` must not be
negative.

### Persistence of debugger data

| Data | `mds_reset` / `mds_load_rom` | `mds_loadstate` | Cleared by |
| --- | --- | --- | --- |
| breakpoints, watchpoints, hits | kept | kept | `mds_point_remove` |
| trace ring | emptied (config kept) | kept (a log of what ran) | `mds_trace` |
| call stacks | cleared | **cleared** (tracking stays on) | `mds_calls(id, 0)` |
| a watchpoint hit not yet taken | cleared | **cleared** | parking on it |
| call counts | kept | kept (a log) | `mds_callcounts_take` |
| coverage | kept | kept (a log) | `mds_coverage_take` |
| last-writer table | zeroed | **zeroed** (tracking stays on) | `mds_lastwriter(id, 0 or 1)` |
| armed step / break requests | cleared | kept (they are requests about the future) | resuming into them |
| packets | kept | kept | `mds_packets` |
| staged rest of a cut-short take | kept | kept | the next take |

The rule for a load (`Debugger::OnStateLoaded`, also used by the Qt frontend): what describes the
execution or memory before the load is dropped (call stacks, the parked thread stacks and
interrupted contexts behind them, a pending watchpoint hit, last-writer records); configuration
(points, trace range, tracking switches, armed steps and break requests) and accumulated logs
(trace, call counts, coverage, packets) are kept. API level 1 kept everything over a load.

---

## 6. Savestates

```c
int mds_savestate(int id, const char* path);
int mds_loadstate(int id, const char* path);
```

Both need an **idle** console: a running or parked one fails with "savestates need a frame
boundary; finish the frame first" (test `test_parked_console_gets_console_running`); finish the
frame with `mds_start(id, 0)` and `mds_wait` first. They run on the emulator thread.

- `mds_savestate` writes melonDS's own format (magic `MELN`), version **14.2**. Errors: "savestate
  failed", "cannot write PATH".
- `mds_loadstate` reads the file, empties the input queue and the mic FIFO, takes a backup of the
  current state, and loads. A rejected file restores the backup ("savestate rejected: PATH"); a
  missing one fails with "cannot read PATH". A load through the socket server also sends an
  `input_queue` event with depth 0.
- **Loading writes the state's save memory into the console's save file.** This is deliberate and
  kept for now: the cart's save memory is part of the emulated console, and after a load the file
  follows what the console now holds (as it does after every save write by the game). It matters
  because a save file booted in place is overwritten by the first load, with the save memory of
  whatever run made the state. **Clients must boot a copy** of any save they want to keep (the
  dq9-re harness copies each save to a temp folder and boots the copy). Future work may make this
  optional.

**Versions.** 14.0 is upstream melonDS. **14.1** (fork commit `d8a5e8d9`) adds the hardware divider
and square-root operands and results (`DIV_NUMER`, `DIV_DENOM`, `DIV_RESULT`, `DIVREM_RESULT`,
`SQRT_PARAM`, `SQRT_RESULT`): the SDK copies the operands into thread contexts, so a 14.0 save, run,
load round trip could diverge. **14.2** (`fix/api-quirks`) adds:

- `KeyInput` (buttons, the pen-down bit and the lid bit; upstream left it out as a TODO),
- the touch controller's pen position,
- the core's mic input buffer (samples already taken from the platform), and
- a headless section `HDLS` after the core's: the held input the empty-queue rule repeats (state,
  source, empty frames since), the pen state the touch timeline ended the frame in, the emulated
  frame count and the samples queued in the mic FIFO.

Loading accepts major 14 with minor 0, 1 or 2. An older state loads as before: what it lacks keeps
its current value (the divider values for 14.0; for 14.0 and 14.1 `KeyInput`, the pen, the held
input, the emulated frame and the mic buffers stay as they were before the load, which empties the
mic buffers). A state from a newer minor is rejected ("state from the future"); builds before this
branch reject 14.2 states. Another frontend's 14.2 state (no `HDLS`) loads; the held input is then
taken from the loaded `KeyInput` and pen position.

What a state contains: the whole emulated console (both CPUs and their banked registers, CP15
and both TCMs, all memory, video, sound, DMA, timers, the RTC, Wi-Fi, the cart and its save memory),
plus (14.2) the input that affects emulation as described above.

What it does not contain (and what happens on load):

- The process frame counter (`mds_frames`), the input log, the input mode and stall timeout, and
  Lua state are frontend state and are not touched by a load.
- Debugger data: a load clears what described the execution before it (call stacks, a pending
  watchpoint hit, last-writer records) and keeps points, logs and armed requests (see the table in
  [section 5](#persistence-of-debugger-data)).
- The framebuffers: the screen shows the pre-load frame until frames have run.
- Caches: this melonDS version emulates no ARM9 data cache, and although instruction-cache arrays
  exist they are not used for fetches (`ICacheLookup` is commented out in `CP15.cpp`), so nothing
  cache-related is missing from a state.
- JIT is off in headless, so states are taken from the interpreter's real pipeline.

Determinism: with 14.1 and later, a save, run, load, run sequence reproduces a run that never loaded
(dq9-re replays all scenarios byte for byte; 14.2 states reproduce the RAM and screen hashes
recorded with 14.1). With 14.2 the input held at the save, the pen and the mic samples also carry
over, so a run that relies on the repeat rule after a load, or feeds the microphone, reproduces too.
Keep to one console per process and set the RTC after loading (the state carries its own clock).

---

## 7. Lua

```c
int mds_lua_run(int id, const char* source, int is_file);
int mds_lua_exec(int id, const char* code);
int mds_lua_stop(int id);
int mds_lua_output(int id, char* out, int max);
```

Each console has one Lua state. All four need a stopped console except `mds_lua_output`
(whose `max` must not be negative).

All three run on the console's **emulator thread** through the command queue, like the other calls
that touch the console, so they are serialised with every other caller (another thread, the socket
server) and the script only ever touches the console from that thread.

- **`mds_lua_run`** discards the current state (stopping any script and removing its hooks),
  creates a fresh one, and loads `source` (a file path when `is_file`, else code) as the main
  coroutine. The body runs **before the call returns**, until its first `emu.frameadvance()` or its
  end; afterwards it is resumed at the end of every frame. A syntax error, and a runtime error in
  that first step, fail the call with the Lua message (the script is then gone). A runtime error in
  a later step is printed to the output (`lua: ...`) and ends the script.
- **`mds_lua_exec`** runs a chunk in the same state (globals shared with the script). A Lua error
  fails the call with the message.
- **`mds_lua_stop`** discards the state, its script and hooks.
- **`mds_lua_output`** removes up to `max` bytes of printed text and returns how many (no NUL
  terminator; loop until 0). Each `print` adds a newline. While a socket client is connected the
  server takes the output (it becomes `lua_output` events).

Order at the end of each frame (emulator thread): `event.onframeend` callbacks, then the script
coroutine is resumed, then the script's input is submitted as one payload.

Functions available (`LuaHost.cpp`, `lua_debug/LuaDebug.cpp`):

| table | functions |
| --- | --- |
| `memory` | `read_u8` `read_s8` `read_u16_le` `read_s16_le` `read_u24_le` `read_s24_le` `read_u32_le` `read_s32_le` `readbyte`, the matching `write_*` and `writebyte`, `read_bytes_as_array(addr, len)`, `write_bytes_as_array(addr, table)`. Optional last argument: `"Main RAM"` (address relative to 0x02000000), a name starting with `ARM7` (ARM7 bus), `ARM9`/`"System Bus"` (ARM9 bus). Same rules as `mds_read`/`mds_write` (I/O reads 0, byte writes). |
| `emu` | `framecount()` (= `mds_frames`), `frameadvance()` (yields; only in the script body), `getsystemid()` ("NDS"/"DSi"), `getconsoleid()` (effective id), `getregister(name, [cpu])`, `getregisters()`, `setregister(name, value, [cpu])` (names `r0`-`r15`, `sp`, `lr`, `pc`, `cpsr`, optional `ARM7 `/`ARM9 ` prefix; `pc` = R15 - 4/2; `cpsr` read-only) |
| `joypad` | `set{A=true, ...}` (key names as in the bit table; nil keys unchanged), `get()` (the pending state if set this step, else the applied one) |
| `input` | `NDSTapDown(x, y)`, `NDSTapUp()` |
| `savestate` | `save(path)`, `load(path)`; they only succeed while the console is idle, i.e. from `mds_lua_exec`/the start of `mds_lua_run`, not from a running script or hook |
| `event` | `onmemoryexecute(fn, addr, [name], [cpu])`, `onmemoryread(fn, addr, [name], [cpu], [size=1])`, `onmemorywrite(...)`, `onframeend(fn, [name])`, `unregisterbyid(id)`, `unregisterbyname(name)` |
| `debugger` | `callstack([cpu])`, `trackcalls(on)`, `callcounts([cpu])` (drains), `coverage(on)`, `draincoverage([cpu])`, `trace([capacity=65536], [cpu], [start], [end])` (one CPU), `draintrace()`, `trackwriters(on)`, `lastwriter(addr)` (returns `pc, cpu` for an instruction, `nil, cpu, channel` for a DMA write, `nil` for none), `pause([cpu])` (break request on one CPU), `step([count=1], [cpu])` (exact, as `mds_step`), `peek(addr, [size=4], [cpu])`; `resume` raises "not available here" in headless. On a console without the debugger all but `peek` and `resume` raise "console N was created without the debugger (debug = 0)" |
| global | `print` (to the output buffer) |

Memory hooks are ordinary debugger points with a callback: they get ids from the same counter,
appear in `mds_point_list`, and need the debugger (on a console without it `event.onmemory*`
raises). The callback runs on the emulator thread at
the access or instruction, with arguments `(addr, value, pc)` (for a DMA write `pc` is the DMA
tag `0xFFFFFFF0 | channel << 1 | arm7`); returning `true` parks the console
like a breakpoint (break kind exec/read/write with the hook's id), anything else continues. That
makes any hook a conditional breakpoint. `event.onframeend` ids are negative. The Lua debugger
tables share their data with the C API (`debugger.draintrace` drains the same ring as
`mds_trace_take`).

---

## 8. GDB stub and socket server

### GDB stub

```c
int mds_gdb(int id, int port9, int port7, int break_on_start);
int mds_gdb_host(int id, const char* host, int port9, int port7, int break_on_start);
```

Needs a stopped console created with `debug != 0` (the stub's checks run only in the debug
interpreter; without the debugger the call fails). It runs on the emulator thread. With both ports
`<= 0` it disables the stub; otherwise it (re)starts one GDB server per CPU on `port9` (ARM9) and
`port7` (ARM7), and `break_on_start` makes both CPUs wait for GDB at their next instruction. A port
`<= 0` next to a valid one is passed to the stub as is.

The stubs listen on **127.0.0.1** (`mds_gdb`, or `mds_gdb_host` with `host` NULL or ""). To debug
from another machine pass an IPv4 address to `mds_gdb_host`: `"0.0.0.0"` for every interface, or one
interface's address. A non-loopback address logs a warning (log level Warn): anyone who can reach
the port controls the emulator, with no authentication. A malformed address fails with "bad host:
...". (API level 1 listened on every interface.) The Qt frontend takes the address from an optional
`Gdb.BindAddress` entry of its config file, also defaulting to 127.0.0.1. While GDB holds a
CPU stopped, the emulator thread is blocked inside the stub, the console still reports
`MDS_RUNNING`, and command-queue calls wait until GDB continues *(inferred)*. Fails with "built
without the GDB stub" when compiled without `ENABLE_GDBSTUB` (on by default). Watchpoints work
through GDB too (README).

### Socket server

```c
int  mds_serve(int port, int flags, int* bound);
int  mds_serve_host(const char* host, int port, int flags, int* bound);
void mds_serve_stop(void);
int  mds_serve_wait(int timeout_ms);
void mds_serve_name(const char* name);
```

The wire format is specified in `src/frontend/headless/Protocol.h` (version 1); this section only
covers the C functions and how serving changes in-process behaviour.

- **`mds_serve`** starts one server thread per process, bound to **127.0.0.1**, serving every
  console in the process. `port = 0` lets the OS pick. Otherwise the first free port in
  `[port, port + range)` is used, `range = flags & 0xFFFF` (0 means 32, 1 means only `port`).
  Returns 0 and writes the bound port to `*bound`, or -1: "bad port", "server already running",
  "no free port in A-B".
- **`mds_serve_host`** is `mds_serve` on another IPv4 address (`host` NULL or "" = 127.0.0.1;
  `"0.0.0.0"` = every interface). A non-loopback address prints a warning to stderr: the protocol
  has no authentication, so anyone who can reach the port controls every console. A malformed
  address fails with "bad host: ...". `melonds_server --host ADDR` uses it (default 127.0.0.1).
- **One client at a time.** A second connection gets a `busy` error and is closed.
- **`mds_serve_stop`** joins the thread and closes the sockets; consoles keep their state.
- **`mds_serve_wait(timeout_ms)`** waits (`< 0` forever) for a client's `shutdown` message:
  returns 1 if one was received since `mds_serve`, else 0. The host then calls `mds_serve_stop` and
  exits (this is what `melonds_server.exe` does).
- **`mds_serve_name`** sets the name returned in the `hello` reply (default "melonds"), before or
  after `mds_serve`.

Effects on the in-process API while the server runs:

- `mds_serve`, every client connect and every disconnect set **every console's** empty-queue mode
  to repeat and stall timeout to 15, and consoles created while serving get the server's options.
- While a client is connected the server thread takes every console's break events and Lua output
  (to send them as events): in-process `mds_take_break` and `mds_lua_output` see nothing. On
  connect, earlier breaks and output are discarded.
- On disconnect the server clears every console's input queue and frame subscriptions and stops the
  consoles that client started.

### Instance base, MAC and save numbering

```c
int mds_set_instance_base(int base);
int mds_instance_base(void);
int mds_mac(int id, uint8_t* out);   /* 6 bytes */
```

The instance base is a process-wide offset added to each console's id to give its *effective id*.
It can only be set while no console exists ("the instance base can only change while no console
exists") and must be `>= 0`. The effective id is used for:

- the firmware MAC address: for effective id `e > 0`, bytes 3, 4, 5 of the firmware's MAC get
  `+e`, `+e * 0x44`, `+e * 0x10` and byte 0 gets its low two bits cleared (`CustomizeFirmware`);
  effective id 0 keeps the firmware's MAC;
- the default save path (`.sav` for 0, `.sav.e+1` otherwise);
- Lua's `emu.getconsoleid()`.

It does not change the ids the API uses or the wireless slot. Its purpose: several processes
(for example one `melonds_server` per player, `--instance-base N`) get distinct MACs and save files.
`mds_mac` copies the console's current firmware MAC. Tests: `test_consoleid.py`.

---

## 9. Python equivalents

Two copies of the ctypes wrapper exist: `tools/melonds-mcp/melonds_mcp/native.py` (`from
melonds_mcp import native`) and `DQ9/dq9-re/melonds/native.py`. With the `fix/api-quirks` update the
dq9-re copy is the same wrapper with its own library lookup (it needs `MELONDS_HEADLESS_LIB`); before
it, the dq9-re copy lacked the input queue and server functions. Errors raise `native.MelonError`
with the `mds_error` text. Methods are on `native.Console` unless marked as module functions.

Functions are bound on first use: the wrapper loads a library that lacks some of them (an older
build), and only calling a missing one raises `MelonError` ("... is not exported by this
melonds_headless library"). `native.has(name)` and `native.api_version()` tell what is there.

| C function | `native.py` |
| --- | --- |
| `mds_version` | `native.api_version()` |
| `mds_has` | `native.has(name)` (on a level-1 library: whether an `mds_*` symbol exists; features are unknown there) |
| `mds_error` | used by every call (message of `MelonError`) |
| `mds_set_local_dir` | not wrapped (`native.lib().mds_set_local_dir(b"dir")`; argtypes are set) |
| `mds_set_log_level` | not wrapped (`native.lib().mds_set_log_level(n)`) |
| `mds_mp_timeout` | `native.mp_timeout(ms=-1)` |
| `mds_set_pacing` | `native.set_pacing(fps)` |
| `mds_create` | `Console(bios9=None, bios7=None, firmware=None, nickname=None, debug=True)` |
| `mds_destroy` | `close()` (also at interpreter exit) |
| `mds_load_rom` | `load_rom(path, save=None)` |
| `mds_reset` | `reset()` |
| `mds_set_rtc` / `mds_get_rtc` | `set_rtc(datetime)` / `rtc()` -> datetime |
| `mds_run` | `run(frames, timeout_ms=60000)` -> `(timed_out, [state])`; `native.run_group(consoles, frames, timeout_ms)` |
| `mds_wait` | `wait(timeout_ms=60000)`; `native.wait_group(consoles, timeout_ms)` |
| `mds_gdb` / `mds_gdb_host` | `gdb(port9=3333, port7=3334, break_on_start=False, host=None)` (`host` given: `mds_gdb_host`) |
| `mds_start` | `start(frames=0xFFFFFFFF)` |
| `mds_stop_after` | `stop_after(frames=0)` |
| `mds_interrupt` | `interrupt()` |
| `mds_state` | `state` (property) |
| `mds_take_break` | `take_break()` -> `Break(kind, cpu, pc, addr, value, size, id, dma)` or None; `kind` is a string from `native.BREAK_KINDS`; a DMA break has `pc >= native.DMA_TAG` |
| `mds_frames` | `frames` (property) |
| `mds_screen` | `screen()` -> `c_uint32` array of 256*384 (`bytes()` gives BGRA) |
| `mds_set_keys` | `set_keys(names or mask)` |
| `mds_touch` | `touch(x=None, y=None)` (None = pen up) |
| `mds_set_lid` | `set_lid(closed)` |
| `mds_input_submit` | `submit_input(buttons=0, lid=False, touch=None, mic=None, mic_level=None, seq=0, client=False)` -> depth; `touch` is `[(offset, x, y, down), ...]` or `(x, y)` |
| `mds_input_submit_multi` | `native.submit_multi([(console, kwargs), ...], client=False)` -> depths |
| `mds_debug_recv_pending` | `native.debug_recv_pending(console, pending)` |
| `mds_input_mode` | `input_mode(mode=None, stall_frames=None)`; mode `"repeat"`, `"neutral"`, `"wait"` |
| `mds_input_depth` / `_waiting` / `_clear` | `input_depth()` / `input_waiting()` / `input_clear()` |
| `mds_input_log` | `input_log(from_frame=0, max=4096)` -> list of dicts |
| `mds_read` / `mds_write` | `read(addr, length, cpu=9)` -> bytes / `write(addr, data, cpu=9)` |
| `mds_get_regs` | `regs(cpu=9)` -> dict `r0`-`r12`, `sp`, `lr`, `pc`, `cpsr`, `spsr`, `r15_raw` |
| `mds_set_reg` | `set_reg(reg, value, cpu=9)`; reg is a name (`r0`-`r15`, `sp`, `lr`, `pc`) or index |
| `mds_bp_add` | `add_breakpoint(addr, cpu=9)` (`cpu="both"` or 3 for both) |
| `mds_wp_add` | `add_watchpoint(addr, size, read=False, write=True, cpu=9)` |
| `mds_point_remove` / `_enable` | `remove_point(id)` / `enable_point(id, enabled)` |
| `mds_point_list` | `points()` -> list of dicts (at most 512) |
| `mds_step` | `step(count=1, cpu=9)` |
| `mds_trace` | `trace(capacity, cpu="both", start=0, end=0xFFFFFFFF)` |
| `mds_trace_take` | `take_trace()` -> list of `(cycle, cpu, pc, instr, cpsr)` (all of it) |
| `mds_calls` | `track_calls(enabled=True)` |
| `mds_callstack` | `callstack(cpu=9)` -> list of dicts, innermost first |
| `mds_callcounts_take` | `take_call_counts(cpu=9)` -> `{entry: count}` (all of it) |
| `mds_coverage` / `_take` | `coverage(enabled=True)` / `take_coverage(cpu=9)` -> sorted list |
| `mds_lastwriter` / `_query` | `track_writers(enabled=True)` / `last_writer(addr)` -> None (no record), `(pc, cpu)` or `("dma", channel, cpu)`; raises `MelonError` for errors (tracking off, running, no debugger). With a level-1 library it falls back to `mds_lastwriter_get`, where errors also give None |
| `mds_savestate` / `mds_loadstate` | `save_state(path)` / `load_state(path)` |
| `mds_lua_run` / `_exec` / `_stop` | `lua_run(source, is_file=False)` / `lua_exec(code)` (returns the output) / `lua_stop()` |
| `mds_lua_output` | `lua_output()` -> str |
| `mds_packets` / `_take` | `packet_log(enabled=True, capacity=4096)` / `take_packets()` -> list of dicts |
| `mds_serve` / `mds_serve_host` (+ `mds_serve_name`) | `native.serve(port=0, port_range=0, name=None, host=None)` -> bound port (`host` given: `mds_serve_host`) |
| `mds_serve_stop` / `mds_serve_wait` | `native.serve_stop()` / `native.serve_wait(timeout_ms=0)` |
| `mds_serve_name` | via `serve(name=...)` |
| `mds_set_instance_base` / `mds_instance_base` | `native.set_instance_base(base)` / `native.instance_base()` |
| `mds_mac` / `mds_save_path` | `mac()` -> 6 bytes / `save_path()` -> str |

`mds_lua_run` raises for an error in the script's first step (API level 2).

---

## 10. Recipes

All recipes use `melonds_mcp.native`; the dq9-re copy works the same.
Paths are examples. Keep one console per process.

### Play inputs and take a screenshot

```python
import shutil
from datetime import datetime
from PIL import Image
from melonds_mcp import native

shutil.copy("saves/game.sav", "work/game.sav")          # melonDS writes to the save it boots
c = native.Console(bios9="bios9.bin", bios7="bios7.bin", firmware="firmware.bin")
c.load_rom("game.nds", save="work/game.sav")
c.set_rtc(datetime(2026, 1, 1, 12, 0, 0))               # games that read the clock replay only with the same clock

seq = [["A"]] * 4 + [[]] * 20 + [["Down"]] * 30          # one entry per frame
for keys in seq:
    c.submit_input(keys)                                 # queue holds 600; run in chunks for longer sequences
timed_out, states = c.run(len(seq))
assert not timed_out and states == [native.IDLE]

px = bytes(c.screen())                                   # 256x384, top screen first, BGRA
Image.frombuffer("RGBA", (256, 384), px, "raw", "BGRA", 0, 1).convert("RGB").save("shot.png")
c.close()
```

With the setters instead of the queue: `c.set_keys(keys); c.run(1)` per frame.

### Branch from a state

```python
c.save_state("work/base.mst")                            # console must be idle
results = {}
for name, first in (("a", ["A"]), ("b", ["B"])):
    c.load_state("work/base.mst")                        # also rewrites the save file from the state
    c.set_rtc(datetime(2026, 1, 1, 12, 0, 0))            # the state carries its own clock
    c.submit_input(first)                                # without it, the keys held at the save repeat (14.2)
    c.submit_input([])
    c.run(120)
    results[name] = c.read(0x020F6D48, 4)                # e.g. gold
c.run(2)                                                 # before trusting screen() after a load
```

### Find who writes an address

```python
c.track_calls(True)                    # enable early: frames from before this are unknown
wp = c.add_watchpoint(0x020F6D48, 4, write=True)
timed_out, (st,) = c.run(600)
if st == native.PARKED:
    b = c.take_break()                 # kind "write"; b.pc = the store; b.value = new value
    who = f"DMA channel {b.dma}" if b.dma >= 0 else f"{b.pc:#x}"     # a DMA write has no pc (b.pc = DMA tag)
    print(f"write {b.value:#x} to {b.addr:#x} by {who} (cpu {b.cpu})")
    for f in c.callstack(b.cpu):       # innermost first
        print(f"  {f['entry']:#010x} called from {f['site']:#010x}{' (exception)' if f['exception'] else ''}")
c.remove_point(wp)
c.run(0)                               # from parked: finish the frame and go idle
print(c.last_writer(0x020F6D48))      # same question after the fact (needs track_writers(True); raises if off)
```

### Log a function's arguments at each call

```python
bp = c.add_breakpoint(0x020D8B94)      # e.g. the file-open function
calls = []
end = c.frames + 600
while c.frames < end:
    timed_out, (st,) = c.run(end - c.frames)   # from parked, run(n) restarts the budget; the partial frame counts
    if st != native.PARKED:
        break
    b = c.take_break()
    if b.kind == "exec" and b.id == bp:
        r = c.regs(9)                  # exact for the CPU that broke
        name = c.read(r["r1"], 64).split(b"\0")[0]
        calls.append((c.frames, r["r0"], name, r["lr"]))
    elif b.kind == "request":
        continue                       # a leftover interrupt request; just resume
c.remove_point(bp)
if c.state == native.PARKED:
    c.run(0)
```

### Collect call counts and coverage for a diff

```python
def profile(state, inputs, frames):
    c.load_state(state)
    c.take_call_counts(9); c.take_coverage(9)   # drop leftovers: loads do not clear them
    c.track_calls(True); c.coverage(True)
    for keys in inputs:
        c.submit_input(keys)
    c.run(frames)
    counts, cov = c.take_call_counts(9), set(c.take_coverage(9))
    c.track_calls(False); c.coverage(False)
    return counts, cov

ca, va = profile("work/base.mst", [["A"]] + [[]] * 59, 60)
cb, vb = profile("work/base.mst", [["B"]] + [[]] * 59, 60)
only_b = {f: n for f, n in cb.items() if f not in ca}        # functions called only in B
changed = {f: (ca[f], cb[f]) for f in ca.keys() & cb.keys() if ca[f] != cb[f]}
new_code = sorted(vb - va)                                   # instructions run only in B (catches tail calls, jump tables)
```

### Read an input log

```python
for e in c.input_log(from_frame=0, max=100000):
    keys = [k for i, k in enumerate(native.KEYS) if e["buttons"] >> i & 1]
    print(e["frame"], e["seq"], keys, "lid" if e["lid"] else "")
# Only consumed payloads are logged: a gap in "frame" means the empty-queue rule applied
# (repeat: the previous state was held).
```

---

## 11. Known issues and surprises

Bugs (not fixed here) and behaviour that is easy to get wrong. *(verified)* items were reproduced
with one console against this branch's DLL. Items fixed in `fix/api-quirks` are listed after the
open ones, with what changed; the open items were renumbered.

1. **`mds_interrupt` leaves a request armed on the other CPU** *(verified)*. It requests a break on
   both CPUs; only the first parks. The next resume parks again at once with a second `request` on
   the other CPU. An interrupt sent to an idle console parks the next run at its first instruction.
   The core has `Debugger::CancelBreakRequests` but the C API never calls it. `mds_wait`/`mds_run`
   interrupting the other consoles of a group leave the same leftovers there.
2. **`mds_get_regs` pc of the non-breaking CPU is wrong while parked** *(verified)*. Index 15 is
   `Debugger::CurPC`, which is 0 or stale for a CPU whose hooks are not armed (only an ARM9
   breakpoint set: ARM7 pc reads 0) and one instruction behind for an armed one. At a DMA break it
   is also only the DMA CPU's last executed instruction. Use `r15_raw`.
3. **`mds_write` silently drops writes to palette, VRAM, OAM and the GBA ROM area on the ARM9 bus**
   *(verified for palette)*. It writes one byte at a time and the DS bus ignores 8-bit writes
   there, yet the call returns 0. Lua `memory.write_*` has the same limitation. I/O cannot be
   written at all, and I/O reads return 0 *(verified)*.
4. **`mds_input_log` `touch_count` is never 0**: a payload without touch points logs 1 (the held
   pen point is counted), unlike the payload header where 0 means "unchanged". The C API cannot
   return the logged touch points or mic samples.
5. **`mds_packets_take` can get stuck**: a staged packet longer than `max_data` is never returned,
   and every call returns 0 while it blocks the rest. *(inferred)*
6. **Lua `savestate.save`/`load` fail inside a running script or hook**: the console is `Running`
   at frame end and inside hooks, and states need an idle console. They only work from
   `mds_lua_exec` or a script body's first step while idle. *(inferred)*
7. **`mds_loadstate` rewrites the save file** from the state's save memory. Deliberate and kept for
   now (see [section 6](#6-savestates)): a save booted in place is overwritten by the first load,
   so clients must boot a copy (the dq9-re harness does).
8. **Loads leave the process counters alone**: `mds_frames` and the input log do not jump on a
   load (the emulated frame that seeds the mic noise does; see [section 3](#microphone-fifo)).
   States older than 14.2 restore no input: after loading one, the keys and pen held before the
   load repeat until a payload arrives.
9. **Serving changes in-process behaviour**: `mds_serve` and each client connect/disconnect reset
   every console's input mode to repeat (stall 15); a connected client takes break events and Lua
   output from in-process callers; a disconnect clears every console's input queue.
10. **Barriers drop consoles that stop**: a console whose frame budget runs out leaves every barrier
    group it is in, including groups for payloads still queued. Running the members one frame at a
    time therefore never synchronises them (intended per the tests, but surprising).
11. **Request breaks served at a frame boundary** (waiting for input or at a barrier) report the raw
    ARM9 R15 as `pc`, not an instruction address.
12. **`mds_stop_after(id, 0)` and `(id, 1)` are the same** while a frame is in progress, and so are
    `mds_start(id, 0)` and `(id, 1)` on a parked console *(verified for start 0)*.
13. **`mds_frames` returns 0 for an unknown id**, indistinguishable from a fresh console;
    `mds_destroy` ignores unknown ids.
14. **Lua memory hooks are plain points**: they show in `mds_point_list` and `mds_point_remove` can
    delete them, leaving the Lua side with a dead id.
15. **`mds_point_list` has no total count**; truncation is only detectable as `n == max`.
16. **A DMA break parks inside the DMA**: between two units, with the CPUs between instructions.
    Commands work as at any park, but the console is not at a frame boundary (no savestates), and
    the program that started the DMA is not recorded (`pc` is the DMA tag).
17. **Savestates 14.2 do not load in older builds** (upstream melonDS and builds before this
    branch reject them as "state from the future"); this build loads 14.0-14.2.
18. **Documentation that disagrees with the code** (dq9-re `docs/playbook.md`, "The emulator
    interfaces" and "How the emulator was changed"): `mds_set_local_dir` is not the BIOS/firmware
    folder (those paths are opened as given); coverage is per instruction, not "every block";
    many calls return counts or ids rather than "0 or a negative error"; the server serves one
    client at a time, not "several clients".

### Fixed in fix/api-quirks

These were items of the level-1 list (old numbers in brackets):

- **`debug = 0` failed silently** [13]: the debugger calls now fail with "console N was created
  without the debugger (debug = 0)" ([section 2](#creating-and-destroying)).
- **Savestates did not carry `KeyInput`; keys held before a load were applied after it; debugger
  data went stale over a load** [15]: savestates 14.2 carry `KeyInput`, the pen, the held input and
  the mic buffers; a load restores the held input and clears call stacks, a pending watchpoint hit
  and last-writer records ([section 6](#6-savestates), [section 5](#persistence-of-debugger-data)).
- **Mic level noise depended on process history** [14]: it is seeded from the emulated frame, which
  states carry ([section 3](#microphone-fifo)).
- **The GDB stub listened on all interfaces** [23]: it listens on 127.0.0.1 unless `mds_gdb_host`
  names another address; the socket server can listen elsewhere on request (`mds_serve_host`,
  `--host`) ([section 8](#8-gdb-stub-and-socket-server)).
- **Negative `max` crashed or misbehaved** [7]: every take/list call and `mds_run`/`mds_wait`'s
  count refuse it.
- **DMA watchpoint breaks reported the CPU's last instruction as `pc` and fired when that CPU next
  executed** [20]: they park at once with `pc` = the DMA tag and the channel in `dma`.
- **`mds_load_rom` accepted a parked console** [12]: it needs an idle one.
- **`mds_lua_run`/`exec`/`stop` ran on the calling thread, and a first-step runtime error returned
  0** [11]: they run on the emulator thread through the command queue, and the error fails the call.
- **`mds_step` from idle executed one instruction fewer than from parked** [3]: `n` executes `n`
  instructions from both ([Stepping](#stepping-mds_step)).
- **`mds_version` was always 1** [9]: it is the API level (2), with `mds_has` for features
  ([Version and features](#version-and-features)).
- **`mds_lastwriter_get` returned 0 for errors as well as "no record"; Lua did not decode DMA
  writers** [6]: `mds_lastwriter_query` separates them (the old call stays as legacy) and Lua
  returns `nil, cpu, channel` for a DMA write ([Last writer](#last-writer)).
