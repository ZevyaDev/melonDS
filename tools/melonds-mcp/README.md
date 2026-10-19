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
| `breakpoint_add`, `watchpoint_add` | execute / read / write points, optional Lua condition |
| `step`, `run`, `wait`, `pause` | execution control, every wait has a timeout |
| `registers`, `memory_read`, `memory_write`, `memory_search`, `disassemble_at` | inspection |
| `calls_track`, `callstack`, `calls_read` | shadow call stack across threads and IRQs, per-function call counts |
| `coverage_track`, `coverage_read` | every executed instruction, grouped by symbol |
| `trace_start`, `trace_read` | instruction ring buffer, or function transitions only |
| `writers_track`, `last_writer` | which instruction last wrote a main-RAM word |
| `packets_track`, `packets_read` | local wireless traffic per console |
| `savestate_save`, `savestate_load`, `screenshot`, `press`, `touch` | state and input |

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
