---
name: melonds-state
description: Save and load emulator savestates and export the applied input log so a scenario can be reproduced. Use to bookmark a game position, return to it for repeated experiments, or record how you got there.
---

# Savestates and input logs

    state save /abs/or/relative/path.state
    state load path.state                 clears the console's input queue

- Savestates are files; the server resolves the path (the CLI makes it absolute first). Keep them in your work
  directory, one per interesting position, with descriptive names (`before_boss.state`).
- Loading restores memory and CPU state but not breakpoints or the server's trace/coverage buffers, so clear or
  re-arm those after loading.
- For repeatable experiments: `state load X`, run the same `input` sequence, compare watches/traces.

## Input log

    input-log                        JSON of every applied payload: frame, buttons, touch, lid, mic
    input-log --from 120 --out run1.inputs.json   save beside the savestate (prints only count)

The log counts frames since the console started; keep it with the savestate it started from. To replay, load
the savestate and re-send the buttons with `input` (one token per distinct stretch).

Take a `screenshot` (melonds-screenshot) when saving a state so you can tell later what it was.
