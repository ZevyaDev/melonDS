# Debugger (Qt frontend)

The Debug menu opens the tools. All of them work on a running or stopped console.

Breakpoints, watchpoints, stepping, tracing, call stacks and coverage need the interpreter: turn
JIT off in Config > Emu settings. With JIT on, the viewers still work and the debugger window says
why the rest is unavailable.

Building needs capstone for disassembly (`pacman -S mingw-w64-ucrt-x86_64-capstone`,
`apt install libcapstone-dev`, `brew install capstone`, or vcpkg). Without it the debugger shows
raw opcodes.

## Symbols

Symbols... in the debugger window's toolbar loads a dsd `config.yaml` (or the folder holding it),
or a text file of `address name [size]` lines. The choice is remembered per game code and shared
by every tool.

- Overlays are detected by comparing RAM with `arm9_overlays/*.bin` from the extract that the
  config's `rom_config` points to; only loaded overlays' names are shown.
- C++ names are demangled (Demangle toggles it). Go to, breakpoint and watch fields accept an
  address, a symbol in either form, or `symbol+0x10`.

## Debugger window

| key | does |
|---|---|
| F6 | break on the selected CPU |
| F5 | continue (also unpauses) |
| F11, F10, Shift+F11 | step into, step over, step out |
| F4 | run to the selected line |
| F9, click in the margin | toggle a breakpoint |
| Ctrl+G | go to address or symbol |
| Enter, double-click | follow a branch or literal |
| Backspace | back |

Double-click a register to change it while stopped. The docks hold both CPUs' registers,
breakpoints and watchpoints (CPU and DMA writes, enable, hits, owner) and the call stack. A Lua
hook that returns true opens the window at the stop.

## Tools

| tool | shows |
|---|---|
| Trace | ring buffer of executed instructions in a PC range, savable |
| Call counts and coverage | calls per function, executed instructions per function with % covered |
| Last writer | the instruction or DMA channel behind the latest write to each main-RAM word |
| RAM search, RAM watch | DeSmuME-style search; typed watches (fx32 too), editable, savable |
| IO registers | ARM9 and ARM7 registers decoded to fields, editable |
| Palettes, Tile viewer, BG map, OAM / sprites | both engines, extended palettes, all BG modes |
| 3D graphics | last frame's polygons and vertices, wireframe with picking, textures in all formats |
| Sound | 16 channels and both capture units, live levels |
| Memory viewer | #2384's viewer plus symbols, last writer, break on read/write, disassemble |
| Lua console | scripts plus a command line evaluated against the live console |

## Lua

The Qt console has #1671's API plus the debugger tables from the headless build: `event.onmemoryexecute`,
`onmemoryread`, `onmemorywrite`, `onframeend`, `emu.getregister`, `setregister`, `getregisters` and
`debugger.*` (`callstack`, `trackcalls`, `callcounts`, `coverage`, `trace`, `trackwriters`,
`lastwriter`, `pause`, `step`, `resume`, `peek`).

Memory hooks run on the emulator thread at the instruction that hit them, so `emu.getregister`
there is exact. `gui`, `forms`, `input`, `client`, `savestate` and `console` are refused inside a
hook: set a variable there and act on it in `_Update`.

## Threading

`debugger/DebugBridge` is the one channel to the emulator thread. `post` and `call` queue work
that runs at the next instruction (interpreter), the next frame (JIT), while paused, or while
stopped at a break; `call` hands the result back on the GUI thread. Nothing on the GUI thread
waits for the emulator thread except `run`, and the emulator thread never waits for the GUI, so
neither side can deadlock the other. Saving or loading a state, reset and similar commands sent
while stopped let the current frame finish first.

`-DMELONDS_AUTOMATION=ON` builds a file-driven command interface (`MELONDS_AUTOMATION=<dir>`) used
to test these windows without a display.
