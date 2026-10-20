---
name: melonds-launch
description: Start, find, inspect and stop a melonds_server emulator process and its consoles. Use first, before any other melonds command, when no server is running yet, when you need its port, or to add/remove consoles, reset or load a ROM.
---

# Launch and attach

All melonds commands are `python -m melonds_mcp.cli <command>` (run from `tools/melonds-mcp`, or use the `melonds`
script). Each prints one JSON document; on failure it prints `{"error": code, "message": ...}` and exits 1.

## Start a server

    python -m melonds_mcp.cli launch --port 0 --name p1 --rom game.nds --port-file p1.port
    -> {"port": 51234, "pid": 4242, "name": "p1", "log": "..."}

- `launch` starts `melonds_server` detached and waits for its ready line. Unknown options go to the server
  (`--consoles N`, `--rom`, `--bios9`, `--firmware`, `--instance-base`, `--name`, ...). The executable comes from
  `--exe`, `$MELONDS_SERVER_EXE`, or the newest `build/*/melonds_server`.
- `--port 0` takes any free port; with the default the server searches upward from 7777. Always use the port the
  command prints (or the `--port-file`), never assume 7777.
- Start one server per group of consoles that need no wireless link. Several consoles that must talk to each other
  share one server (`--consoles 2`).

## Tell the other commands where the server is

Pass `--port N` or `--port-file FILE` to each command, or set `MELONDS_PORT=N` once in the shell. Add
`--console ID` to address a console other than the lowest one.

## Inspect and control

    attach                       name, port, pid, consoles with state (idle|running|parked) and frame count
    consoles list|create|destroy ID
    reset            rom-load ROM [--save SAV]
    shutdown                     ends the server process

The server serves one client at a time; commands are short-lived, so just run them one after another. A
`busy` error means another client is connected: wait and retry.

Console state, breakpoints and Lua scripts persist between commands. Subscriptions, live mode and queued input
do not, which is why `input` and `live start` run for a bounded time in one command.

Always `shutdown` servers you launched when finished; stray processes lock the DLLs.
