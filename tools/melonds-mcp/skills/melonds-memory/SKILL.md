---
name: melonds-memory
description: Read, write and search emulated DS memory and registers, find on-screen text in RAM, and keep per-game named watches. Use when you need a game variable's value, want to poke memory, locate a value that changed, or track named addresses across sessions.
---

# Memory, registers, watches

Commands: `python -m melonds_mcp.cli <command>`; server selection as in melonds-launch. All addresses accept
`0x02200000`, decimal, or `Symbol[+0x10]` (needs symbols, see melonds-analysis). `--cpu 9|7` picks the ARM9
(default) or ARM7 bus. Reads and writes see a whole frame; no need to pause first.

    read 0x02200000 16                      {"hex": "...", "addr", "length"}
    read PlayerHp 4 --as u32                also decodes: u8 s8 u16 s16 u32 s32 fx32 fx16 ascii utf16
    read 0x02000000 0x400000 --out ram.bin  raw bytes to a file
    write 0x02200000 0x12345678             --size 1|2|4 (default 4)
    write 0x02200000 --hex deadbeef         or --file blob.bin

## Find things

    search 100 --size 2                     aligned 2-byte value in main RAM (server-side scan)
    search --bytes 4a6f686e                 byte string at any offset
    find-text "Potion"                      ascii, utf16 and utf8 hits; --enc utf16 to narrow
    search 100 --size 4 --with-symbols      adds the symbol covering each hit

Narrowing a value that changes: `search` once, do something in the game (`input`), `search` again with the new
value and intersect the address lists yourself. `--start/--length/--limit` bound the scan; `truncated: true`
means more hits exist.

## Registers

    regs [--cpu 7]                          r0-r12 sp lr pc cpsr spsr (+ pc_symbol with symbols)
    set-reg r0 5      set-reg pc FuncName   changes take effect on the next run

## Per-game watches

Named typed addresses stored per game (key = game code from the cart header; `nocart` when none; `--game KEY`
overrides). They live in `$MELONDS_CLI_HOME/watches/<GAME>.json` and survive across sessions and servers.

    watch add hp 0x02200000 --type u16 --note "player hp"
    watch add speed PlayerSpeed --type fx32       fx32 = 20.12 fixed point, shown as a float
    watch add name 0x02201000 --type ascii --length 16
    watch list        watch read        watch read hp speed        watch remove hp

Types: u8 s8 u16 s16 u32 s32 fx32 fx16 ascii utf16 bytes. A good loop is `input ...` then `watch read`.
