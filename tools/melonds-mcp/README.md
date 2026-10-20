# melonDS headless debugger and MCP server

`melonds_headless` is melonDS without a window: several consoles in one process, each on its own
thread, with a debugger, Lua, and local wireless between them. `melonds_mcp` drives it over MCP.

## Build

Windows (MSYS2 UCRT64), Linux and macOS:

    cmake -B build/headless -G Ninja -DBUILD_QT_SDL=OFF -DBUILD_HEADLESS=ON -DENABLE_OGLRENDERER=OFF
    cmake --build build/headless
    pip install -e tools/melonds-mcp

The server finds `build/*/melonds_headless.*` on its own; `MELONDS_HEADLESS_LIB` overrides it.
On Windows it adds `C:\msys64\ucrt64\bin` to the DLL path (`MELONDS_DLL_DIRS` overrides it).

## MCP client config

    {
      "mcpServers": {
        "melonds": {
          "command": "python",
          "args": ["-m", "melonds_mcp"],
          "cwd": "<repo>/tools/melonds-mcp",
          "env": {
            "MELONDS_ROM": "<rom.nds>",
            "MELONDS_SYMBOLS": "<decomp>/config/usa/arm9/config.yaml"
          }
        }
      }
    }

`MELONDS_ROM` creates console 0 with that ROM. `MELONDS_SYMBOLS` loads a dsd `config.yaml`
(overlay symbols resolve only while that overlay is in RAM) or an `addr name [size]` file.
`MELONDS_BIOS9`, `MELONDS_BIOS7`, `MELONDS_FIRMWARE` select dumps; without them FreeBIOS and
generated firmware are used. `MELONDS_LOCAL_DIR` is where melonDS keeps its own files.

## Debugger

Breakpoints and watchpoints stop the console inside the CPU loop; registers, memory and the call
stack can be read while it is stopped. `run` with several consoles runs them together, which is
what local multiplayer needs, and a stop in one stops the others at their next instruction.

| tool | does |
|---|---|
| `breakpoint_add`, `watchpoint_add` | execute / read / write points (CPU and DMA writes), optional Lua condition |
| `step`, `run`, `wait`, `pause` | execution control, every wait has a timeout |
| `live_start`, `live_stop`, `pacing` | free-running consoles at 60 fps; local wireless sessions need this |
| `registers`, `memory_read`, `memory_write`, `memory_search`, `disassemble_at` | inspection |
| `ramsearch_start`, `ramsearch_filter`, `ramsearch_list` | DeSmuME-style RAM search: snapshot, then changed / increased / equal / delta |
| `watch_add`, `watch_read`, `watch_save`, `watch_load` | named, typed addresses (fx32 included), shareable as JSON; one list per game (see below) |
| `symbols_load`, `symbol_lookup`, `overlays_loaded` | per-console symbol tables |
| `text_find` | find on-screen text in RAM (ASCII, UTF-16, UTF-8) |
| `input_sequence`, `press`, `touch` | input; `"A*4 w20 Down*30"` plays a whole sequence in one call |
| `calls_track`, `callstack`, `calls_read` | shadow call stack across threads and nested IRQs, per-function call counts |
| `coverage_track`, `coverage_read` | every executed instruction, grouped by symbol |
| `trace_start`, `trace_read` | instruction ring buffer, or function transitions only |
| `writers_track`, `last_writer` | which instruction (or DMA channel) last wrote a main-RAM word |
| `packets_track`, `packets_read` | local wireless traffic per console |
| `savestate_save`, `savestate_load`, `screenshot` | state; savestates need a frame boundary |

Symbols and watches are per console. `symbols_load(path, console=0)` fills that console's table, and
every tool that prints or parses addresses uses the table of the console it targets, so two consoles
can run different games or different overlays. `MELONDS_SYMBOLS` applies to the `MELONDS_ROM`
console (console 0); other consoles start with no symbols unless `console_create(symbols=...)` or
`symbols_load` gives them some. Destroying a console drops its table. A watch list belongs to a game:
consoles whose ROM path (made absolute) is the same share one list, consoles with different ROMs do
not, and a console without a ROM has its own. The list is dropped when no console runs that game any
more. `watch_save` and `watch_load` take `console` and use that console's list; the JSON format is unchanged.

The GDB stub (`mds_gdb`, ARM9 3333 / ARM7 3334) also works in the headless build, watchpoints included.

## Lua

BizHawk-style API, the same names as the Qt Lua console where they overlap.

| table | functions |
|---|---|
| `memory` | `read_u8` `read_s8` `read_u16_le` `read_s16_le` `read_u24_le` `read_u32_le` `read_s32_le` `write_*` `read_bytes_as_array` `write_bytes_as_array`; optional domain `"ARM9 System Bus"`, `"ARM7 System Bus"`, `"Main RAM"` |
| `emu` | `frameadvance` `framecount` `getsystemid` `getregister` `getregisters` `setregister` |
| `event` | `onmemoryexecute(fn, addr, [name], [domain])` `onmemoryread(fn, addr, [name], [domain], [size])` `onmemorywrite(...)` `onframeend(fn)` `unregisterbyid` `unregisterbyname` |
| `debugger` | `callstack` `trackcalls` `callcounts` `coverage` `draincoverage` `trace` `draintrace` `trackwriters` `lastwriter` `pause` `step` `peek` |
| `joypad`, `input`, `savestate` | `set` `get`, `NDSTapDown` `NDSTapUp`, `save` `load` |

Hook callbacks receive `(addr, value, pc)`. Returning `true` stops the console, which makes any
hook a conditional breakpoint. The script body runs as a coroutine; `emu.frameadvance()` yields
one frame.
