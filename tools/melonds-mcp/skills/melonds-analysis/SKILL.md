---
name: melonds-analysis
description: Reverse-engineer game code with melonds commands: breakpoints and watchpoints, instruction traces, call stacks and call counts, coverage, last-writer queries, wireless packet logs, and symbol files. Use to find which function does something, who writes a variable, or what code an action runs.
---

# Analysis

Commands: `python -m melonds_mcp.cli <command>`. Everything except packets needs the console stopped
(`idle` after `run`, or `parked` at a break); otherwise you get `console_running`. All need the interpreter
(no JIT).

## Symbols

    symbols load config/arm9.txt     server-side table + remembered for name arguments of this console
    symbols lookup 0x02004c10        -> {"name": "Func+0x4"};   symbols lookup Func -> address
    symbols clear

Plain files are `ADDR NAME [SIZE]` lines. After loading, any address argument accepts `Name` or `Name+0x10`,
and `--with-symbols` adds server-side names to `trace read`, `calls`, `coverage read`, `search`, `find-text`.
`--symbols FILE` on any command uses a file for that call only. dsd `config.yaml` needs PyYAML.

## Break on something

    points bp Func                       execution breakpoint (--cpu 9|7|both)
    points wp PlayerHp 4 --write         watchpoint; add --read for reads too
    run 600                              -> "breaks": [{"kind": "exec"|"write"|..., "pc", "addr", "value"}]
    regs ; calls stack --with-symbols    inspect while parked
    step --count 1                       single-step; points list / enable / disable / remove ID

## What ran

    trace config --capacity 4096 --start 0x02004000 --end 0x02005000
    run 1 ;  trace read --max 200 --with-symbols       (reading clears it)
    coverage track on ;  <action> ;  coverage read --with-symbols      addresses executed since last read
    calls track on ;  run 1 ;  calls counts --with-symbols             functions called and how often
    calls stack                                                        shadow call stack when parked

Trick for naming a function: `calls counts` (clear), do the action with `input`, `calls counts` again; the new
entries are what the action ran.

## Who wrote this

    last-writer track on ;  run 60 ;  last-writer get PlayerHp
    -> {"writer": {"kind": "cpu", "pc": ...}}  or {"kind": "dma", "channel": ...}; null if untouched

Then `points bp` on that pc, or `read pc-8 32` and disassemble.

## Wireless

    packets track on --capacity 4096 ;  run ... ;  packets read      (works while running)
