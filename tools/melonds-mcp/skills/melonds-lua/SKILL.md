---
name: melonds-lua
description: Run Lua scripts or snippets inside a melonds console (memory hooks, per-frame logic, conditional breakpoints, joypad control). Use when a task needs logic running every frame or on a memory access, which separate CLI calls cannot do.
---

# Lua

Each console has its own Lua state; scripts cannot see other consoles. Commands: `python -m melonds_mcp.cli lua ...`.

    lua exec "print(memory.read_u32_le(0x02200000))"        run a snippet, prints {"output": "..."}
    lua run script.lua --frames 120                         start a file, run 120 frames, return its output
    lua run "print(1)" --inline
    lua stop                                                end the console's script and remove its hooks

Hooks and `emu.frameadvance()` loops only advance while the console runs, so pair `lua run` with `--frames N`
or follow with `run N`. Hooks stay installed between commands until `lua stop`. Output printed while later
commands run is not shown; use `--frames` and `--wait SECONDS` on the command that triggers it.

API (BizHawk style): `memory.read_u8/u16_le/u32_le`, `memory.write_*`, `emu.framecount()`, `emu.getregister`,
`event.onmemoryexecute(fn, addr)`, `event.onmemorywrite(fn, addr)`, `event.onframeend(fn)`,
`joypad.set({A=true})`, `emu.getconsoleid()`. A hook returning `true` stops the console like a breakpoint:
check `run`'s `breaks` or `attach` (state `parked`).

Example, log every write to a variable with the writing pc:

    lua run "event.onmemorywrite(function(a,v,pc) print(string.format('%08x <- %x at %08x', a, v, pc)) end, 0x02200000)" --inline --frames 300
