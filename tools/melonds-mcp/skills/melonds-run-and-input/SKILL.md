---
name: melonds-run-and-input
description: Advance the emulator and press buttons or touch the screen with melonds commands. Use to run frames, play a button sequence like "A*4 w20 Down*30", step instructions, pause, run live for a while, or replay recorded input.
---

# Run and input

Needs a server (see melonds-launch). Commands: `python -m melonds_mcp.cli <command>`.

## Run frames

    run 60                        runs 60 frames, waits, prints state and any breaks
    run 60 --consoles 0,1         runs consoles together (needed for local wireless)
    live start --frames 300       free-run for 300 frames or --seconds 5, then stop
    pause        step --count 5 [--cpu 7]

`run` returns `{"frames", "consoles": [{id, state, frames}], "breaks": [...]}`. A console with state `parked`
stopped on a breakpoint or watchpoint; `breaks` says which (kind, pc, addr). `run` more frames to continue.

## Send input

    input "A*4 w20 Down*30"
    input "touch:128,96*3 w10"
    input "A+B*2 Up+touch:10,20*5" --frames 120 --mode repeat

Grammar: tokens split on spaces or commas; `Key`, `Key+Key`, `*N` frames to hold (default 4), `wN` N frames
with nothing pressed (keys: A B Select Start Right Left Up Down R L X Y, any case); `touch:X,Y` pen down
(X 0-255, Y 0-191), alone or combined with keys. A touch ends at the first token without one, and a sequence
ending on a touch gets one extra release frame.

- `input` sends one payload per frame and runs exactly that many frames in the same command (the queue is
  cleared when a command disconnects, so queuing alone is useless; `--no-run` exists for tests).
- `--mode` decides what frames after the sequence see (only matters with `--frames N` longer than the sequence):
  `wait` (default, deterministic), `repeat` (hold last), `neutral` (release).
- Output: `payloads`, `frames_run`, `frame`, `breaks`. If a break fires mid-sequence the command stops early.
- Input is applied exactly once per frame, so a replay with the same sequence from the same savestate is
  reproducible. Save the applied log with `input-log --out FILE` next to your savestate (see melonds-state).

Menus and dialogue usually need a key held 2-4 frames and released 2-4 frames: `A*3 w6 A*3 w6`. Check the
result with `screenshot` or a watch (melonds-memory) rather than assuming the input registered.
