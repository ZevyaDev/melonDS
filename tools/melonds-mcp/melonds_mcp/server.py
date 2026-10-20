import io
import os
import struct
from pathlib import Path

from PIL import Image as PILImage

try:
    from mcp.server.mcpserver import Image, MCPServer as Server
except ImportError:
    from mcp.server.fastmcp import FastMCP as Server, Image

from . import native
from .disasm import disassemble, format_lines
from .native import Console, MelonError, PARKED, RUNNING
from .ramtools import COMPARES, RamSearch, Watches, parse_sequence, text_search
from .symbols import SymbolTable

server = Server(
    "melonds",
    instructions=(
        "Headless melonDS with a debugger. Several consoles can run in one process and play local "
        "wireless multiplayer with each other. Addresses accept numbers, hex strings or symbol names "
        "once symbols_load has run. A breakpoint or watchpoint stops every console in the run group; "
        "`run` with frames=0 resumes a stopped console to the end of its current frame."
    ),
)

CONSOLES: dict[int, Console] = {}
SEARCHES: dict[int, RamSearch] = {}
SYMBOLS: dict[int, SymbolTable] = {}  # per console id; a console without symbols_load has no entry
WATCH_LISTS: dict[tuple, Watches] = {}  # per game, see watch_key
NO_SYMBOLS = SymbolTable()  # stays empty
STATE_NAMES = {0: "idle", 1: "running", 2: "stopped"}
HEX_DIGITS = set("0123456789abcdefABCDEF")


def con(console):
    if console not in CONSOLES:
        raise MelonError(f"no console {console}; call console_create first")
    return CONSOLES[console]


def syms(c):
    """The symbol table of console `c` (an empty one when it has none)."""
    return SYMBOLS.get(c.id, NO_SYMBOLS) if c else NO_SYMBOLS


def watch_key(c):
    """Watch lists belong to a game: consoles with the same ROM path (absolute, case-normalised) share one list.
    A console without a ROM has a list of its own."""
    if c.rom:
        return ("rom", os.path.normcase(os.path.abspath(c.rom)))
    return ("console", c.id)


def watches_of(c, create=False):
    key = watch_key(c)
    if create:
        return WATCH_LISTS.setdefault(key, Watches())
    return WATCH_LISTS.get(key) or Watches()


def prune_watches():
    """Drop watch lists that no console's game refers to any more."""
    live = {watch_key(c) for c in CONSOLES.values()}
    for key in [k for k in WATCH_LISTS if k not in live]:
        del WATCH_LISTS[key]


def addr_of(value, c=None):
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    if len(text) == 8 and set(text) <= HEX_DIGITS:
        return int(text, 16)
    if text.isdigit():
        return int(text)
    s = syms(c).resolve(text, c)
    if not s:
        raise MelonError(f"unknown address or symbol: {text}")
    return s.addr


def describe(addr, c=None):
    t = syms(c)
    return t.describe(addr, c) if t.source else f"{addr:08x}"


def is_thumb(addr, c, cpu, mode):
    if mode in ("thumb", "arm"):
        return mode == "thumb"
    if syms(c).source:
        s = syms(c).lookup(addr, c)
        if s and s.kind == "function":
            return s.thumb
    regs = c.regs(cpu)
    return bool(regs["cpsr"] & 0x20) and addr == regs["pc"]


def disasm_text(c, addr, count, cpu, mode="auto", mark=None):
    thumb = is_thumb(addr, c, cpu, mode)
    code = c.read(addr, count * (2 if thumb else 4), cpu)
    lines = disassemble(code, addr, thumb, lambda a: describe(a, c))
    return format_lines(lines, (lambda a: describe(a, c)) if syms(c).source else None, mark)


def break_report(c):
    b = c.take_break()
    if not b:
        return None
    report = {"kind": b.kind, "cpu": b.cpu, "pc": f"{b.pc:08x}", "at": describe(b.pc, c)}
    if b.kind in ("read", "write"):
        report |= {"addr": f"{b.addr:08x}", "size": b.size, "value": f"{b.value:x}", "data": describe(b.addr, c)}
        if b.dma >= 0:
            report["by"] = f"DMA channel {b.dma} (pc is the last instruction this CPU ran)"
    if b.id:
        report["point"] = b.id
    try:
        report["code"] = disasm_text(c, b.pc, 3, b.cpu, mark=b.pc)
    except MelonError:
        pass
    return report


def group_result(group, timed_out, states):
    out = {}
    for c, st in zip(group, states):
        entry = {"state": STATE_NAMES[st], "frames": c.frames}
        if st == PARKED and (b := break_report(c)):
            entry["break"] = b
        out[c.id] = entry
    if timed_out:
        out["timed_out"] = "consoles still running; call wait or pause"
    return out


@server.tool()
def console_create(nickname: str = "", bios9: str = "", bios7: str = "", firmware: str = "", rom: str = "", save: str = "", symbols: str = "") -> dict:
    """Create a console (debug interpreter). Empty BIOS/firmware use FreeBIOS and generated firmware. Optional rom loads it.
    Optional symbols is loaded into this console's symbol table (as symbols_load); new consoles otherwise start without symbols."""
    c = Console(bios9 or os.environ.get("MELONDS_BIOS9"), bios7 or os.environ.get("MELONDS_BIOS7"),
                firmware or os.environ.get("MELONDS_FIRMWARE"), nickname or None)
    CONSOLES[c.id] = c
    if rom:
        c.load_rom(rom, save or None)
    if symbols:
        symbols_load(symbols, c.id)
    return {"console": c.id, "rom": c.rom}


@server.tool()
def console_destroy(console: int) -> str:
    """Destroy a console, dropping its symbol table (and its game's watches when no other console runs that game)."""
    con(console).close()
    del CONSOLES[console]
    SYMBOLS.pop(console, None)
    prune_watches()
    return "destroyed"


@server.tool()
def console_list() -> dict:
    """List consoles with state, frame count, ROM and both CPUs' PCs."""
    out = {}
    for i, c in CONSOLES.items():
        entry = {"state": STATE_NAMES[c.state], "frames": c.frames, "rom": c.rom}
        if c.state != RUNNING:
            entry["arm9"] = describe(c.regs(9)["pc"], c)
            entry["arm7"] = f"{c.regs(7)['pc']:08x}"
        out[i] = entry
    return out


@server.tool()
def rom_load(path: str, console: int = 0, save: str = "") -> str:
    """Load a ROM into a console and boot it (direct boot). save defaults to <rom>.sav (.sav.N for console N>0).
    The console then uses the watch list of that ROM's game."""
    con(console).load_rom(path, save or None)
    prune_watches()
    return f"loaded {path}"


@server.tool()
def console_reset(console: int = 0) -> str:
    """Reset and reboot the loaded ROM."""
    con(console).reset()
    return "reset"


@server.tool()
def run(frames: int = 1, consoles: list[int] | None = None, timeout_s: float = 60) -> dict:
    """Run consoles together for N frames (all of them run concurrently, needed for local multiplayer).
    Stops early when any console hits a breakpoint/watchpoint/step; the others stop at their next instruction.
    frames=0 on a stopped console finishes its current frame."""
    group = [con(i) for i in (consoles or [0])]
    timed_out, states = native.run_group(group, frames, int(timeout_s * 1000))
    return group_result(group, timed_out, states)


@server.tool()
def pacing(fps: int = 0) -> str:
    """Run every console at most `fps` frames per wall-clock second (0 = as fast as possible). Local wireless between
    consoles needs real-time pacing (60) like the Qt frontend; everything else runs fastest unpaced."""
    native.set_pacing(fps)
    return f"pacing {fps or 'off'}"


@server.tool()
def live_start(consoles: list[int] | None = None, fps: int = 60) -> str:
    """Run consoles continuously at `fps` (local wireless sessions need this: a console that stops stalls the others).
    Input, screenshots and memory reads work while live; stop with live_stop."""
    native.set_pacing(fps)
    for i in consoles or [0]:
        con(i).start()
    return "live"


@server.tool()
def live_stop(consoles: list[int] | None = None, timeout_s: float = 30) -> dict:
    """Stop live consoles at their next frame boundary and turn pacing off."""
    group = [con(i) for i in (consoles or [0])]
    for c in group:
        c.stop_after(0)
    timed_out, states = native.wait_group(group, int(timeout_s * 1000))
    native.set_pacing(0)
    return group_result(group, timed_out, states)


@server.tool()
def wait(consoles: list[int] | None = None, timeout_s: float = 60) -> dict:
    """Wait for running consoles to finish or stop."""
    group = [con(i) for i in (consoles or [0])]
    timed_out, states = native.wait_group(group, int(timeout_s * 1000))
    return group_result(group, timed_out, states)


@server.tool()
def pause(console: int = 0) -> str:
    """Stop a running console at its next instruction."""
    con(console).interrupt()
    return "interrupt requested"


@server.tool()
def step(count: int = 1, cpu: int = 9, console: int = 0) -> dict:
    """Execute `count` instructions on one CPU (the other keeps time), then stop. Returns the code at the new PC."""
    c = con(console)
    c.step(count, cpu)
    timed_out, states = native.run_group([c], 120, 30000)
    return group_result([c], timed_out, states)


@server.tool()
def registers(cpu: int = 9, console: int = 0) -> dict:
    """Read registers. pc is the address of the next instruction to execute."""
    c = con(console)
    regs = c.regs(cpu)
    out = {k: f"{v:08x}" for k, v in regs.items() if k != "r15_raw"}
    out["mode"] = {0x10: "usr", 0x11: "fiq", 0x12: "irq", 0x13: "svc", 0x17: "abt", 0x1B: "und", 0x1F: "sys"}.get(regs["cpsr"] & 0x1F, "?")
    out["thumb"] = bool(regs["cpsr"] & 0x20)
    out["at"] = describe(regs["pc"], c)
    return out


@server.tool()
def set_register(name: str, value: str, cpu: int = 9, console: int = 0) -> str:
    """Write r0-r15/sp/lr/pc. Writing pc flushes the pipeline."""
    c = con(console)
    c.set_reg(name, addr_of(value, c), cpu)
    return "ok"


@server.tool()
def memory_read(address: str, length: int = 64, cpu: int = 9, console: int = 0, width: int = 1) -> str:
    """Hexdump memory (no side effects; IO registers read as 0). width 1/2/4 groups little-endian values."""
    c = con(console)
    addr = addr_of(address, c)
    data = c.read(addr, length, cpu)
    rows = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        if width == 1:
            vals = " ".join(f"{b:02x}" for b in chunk)
        else:
            fmt = {2: "<H", 4: "<I"}[width]
            vals = " ".join(f"{struct.unpack_from(fmt, chunk, i)[0]:0{width * 2}x}" for i in range(0, len(chunk) - width + 1, width))
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        rows.append(f"{addr + off:08x}  {vals:<48}  {text}")
    return "\n".join(rows)


@server.tool()
def memory_write(address: str, hex_bytes: str, cpu: int = 9, console: int = 0) -> str:
    """Write raw bytes (hex string) to memory. Refuses IO registers."""
    c = con(console)
    data = bytes.fromhex(hex_bytes.replace(" ", ""))
    c.write(addr_of(address, c), data, cpu)
    return f"wrote {len(data)} bytes"


@server.tool()
def memory_search(value: str, size: int = 4, start: str = "0x02000000", length: int = 0x400000, console: int = 0, limit: int = 64) -> list[str]:
    """Find a little-endian value (size 1/2/4) or a hex byte string (size 0) in memory."""
    c = con(console)
    base = addr_of(start, c)
    needle = bytes.fromhex(value.replace(" ", "")) if size == 0 else addr_of(value, c).to_bytes(size, "little")
    data = c.read(base, length)
    hits, pos = [], data.find(needle)
    while pos >= 0 and len(hits) < limit:
        if size == 0 or pos % size == 0:
            hits.append(f"{base + pos:08x} {describe(base + pos, c)}")
        pos = data.find(needle, pos + 1)
    return hits


@server.tool()
def disassemble_at(address: str = "pc", count: int = 16, cpu: int = 9, mode: str = "auto", console: int = 0) -> str:
    """Disassemble with symbolized branch targets. mode: auto (symbol/CPSR), arm, thumb. address 'pc' = current PC."""
    c = con(console)
    pc = c.regs(cpu)["pc"]
    addr = pc if address == "pc" else addr_of(address, c)
    return disasm_text(c, addr, count, cpu, mode, mark=pc)


@server.tool()
def callstack(cpu: int = 9, console: int = 0) -> list[str]:
    """Shadow call stack (innermost first). Needs calls_track(true) before the calls happened."""
    c = con(console)
    out = []
    for f in c.callstack(cpu):
        kind = "exception" if f["exception"] else f"called from {describe(f['site'], c)}"
        out.append(f"{describe(f['entry'], c)}  ({kind}, returns to {f['ret']:08x})")
    return out


@server.tool()
def screenshot(console: int = 0) -> Image:
    """Both screens as one 256x384 PNG (top above bottom) from the last completed frame."""
    px = bytes(con(console).screen())
    img = PILImage.frombuffer("RGBA", (256, 384), px, "raw", "BGRA", 0, 1).convert("RGB")
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return Image(data=buf.getvalue(), format="png")


@server.tool()
def press(buttons: list[str], frames: int = 6, release_frames: int = 6, console: int = 0) -> dict:
    """Hold buttons (A B X Y L R Start Select Up Down Left Right) for N frames, then release for M frames."""
    c = con(console)
    c.set_keys(buttons)
    timed_out, states = native.run_group([c], frames, 60000)
    c.set_keys([])
    if states[0] != PARKED and release_frames:
        timed_out, states = native.run_group([c], release_frames, 60000)
    return group_result([c], timed_out, states)


@server.tool()
def input_sequence(steps: str, consoles: list[int] | None = None) -> dict:
    """Play inputs in one call: tokens like "A*4 w20 Down*30 A+B*2 w10" (buttons*frames held, wN waits N frames).
    Runs all listed consoles together, so it also drives multiplayer. Stops early on a break."""
    group = [con(i) for i in (consoles or [0])]
    timed_out, states = False, [0] * len(group)
    for keys, frames in parse_sequence(steps):
        for c in group:
            c.set_keys(keys)
        timed_out, states = native.run_group(group, frames, 120000)
        if PARKED in states or timed_out:
            break
    for c in group:
        c.set_keys([])
    return group_result(group, timed_out, states)


@server.tool()
def ramsearch_start(start: str = "0x02000000", length: int = 0x400000, size: int = 4, signed: bool = True, cpu: int = 9, console: int = 0) -> dict:
    """Begin a RAM search: snapshot every aligned value in the range. Then change the game state and narrow
    with ramsearch_filter, like DeSmuME's RAM Search."""
    c = con(console)
    SEARCHES[console] = RamSearch(c, addr_of(start, c), length, size, signed, cpu)
    return {"candidates": len(SEARCHES[console].cand)}


@server.tool()
def ramsearch_filter(compare: str, value: int | None = None, console: int = 0, show: int = 20) -> dict:
    """Keep candidates whose value, compared with the previous snapshot (or with `value`), matches:
    changed, unchanged, increased, decreased, eq, ne, gt, lt, ge, le, delta (current - previous == value)."""
    s = SEARCHES.get(console)
    if not s:
        raise MelonError("call ramsearch_start first")
    n = s.filter(compare, value)
    return {"candidates": n, "first": [f"{a:08x} {cur} (was {prev}) {describe(a, s.console)}" for a, cur, prev in s.rows(show)]}


@server.tool()
def ramsearch_list(limit: int = 100, console: int = 0) -> list[str]:
    """Current RAM search candidates with current and previous values."""
    s = SEARCHES.get(console)
    if not s:
        raise MelonError("call ramsearch_start first")
    return [f"{a:08x} {cur} (was {prev}) {describe(a, s.console)}" for a, cur, prev in s.rows(limit)]


@server.tool()
def watch_add(name: str, address: str, type: str = "u32", cpu: int = 9, length: int = 0, note: str = "", console: int = 0) -> dict:
    """Name an address in the watch list of the console's game (consoles running the same ROM share it). Types: u8 s8 u16 s16 u32 s32 fx32 fx16 (DS fixed point /4096), bytes, ascii, utf16 (with length)."""
    c = con(console)
    watches_of(c, True).add(name, addr_of(address, c), type, cpu, length, note)
    return watch_read(console)


@server.tool()
def watch_remove(name: str, console: int = 0) -> dict:
    """Remove a named watch."""
    watches_of(con(console)).remove(name)
    return watch_read(console)


@server.tool()
def watch_read(console: int = 0) -> dict:
    """Current values of all watches."""
    c = con(console)
    return watches_of(c).read(c)


@server.tool()
def watch_save(path: str, console: int = 0) -> str:
    """Save the console's game watch list as JSON, to share findings."""
    w = watches_of(con(console))
    w.save(path)
    return f"saved {len(w.items)} watches"


@server.tool()
def watch_load(path: str, console: int = 0) -> list[str]:
    """Load (merge) a watch list saved by watch_save into the console's game list."""
    return watches_of(con(console), True).load(path)


@server.tool()
def text_find(text: str, start: str = "0x02000000", length: int = 0x400000, console: int = 0) -> list[str]:
    """Find a string in memory as ASCII, UTF-16LE or UTF-8 (e.g. on-screen dialogue to locate the text buffer)."""
    c = con(console)
    return [f"{a:08x} {enc} {describe(a, c)}" for a, enc in text_search(c, text, addr_of(start, c), length)]


@server.tool()
def touch(x: int, y: int, frames: int = 6, console: int = 0) -> dict:
    """Touch the bottom screen at (x, y) for N frames, then release."""
    c = con(console)
    c.touch(x, y)
    timed_out, states = native.run_group([c], frames, 60000)
    c.touch()
    return group_result([c], timed_out, states)


@server.tool()
def breakpoint_add(address: str, cpu: int = 9, condition: str = "", console: int = 0) -> dict:
    """Execution breakpoint. condition: optional Lua expression (e.g. "emu.getregister('r0') == 3");
    the console stops only when it is true. Conditional breakpoints live in the console's Lua state."""
    c = con(console)
    addr = addr_of(address, c)
    if condition:
        dom = "ARM7" if cpu == 7 else "ARM9"
        out = c.lua_exec(f"print(event.onmemoryexecute(function(addr, val, pc) return ({condition}) end, {addr}, 'mcp', '{dom}'))")
        point = int(out.split()[-1])
    else:
        point = c.add_breakpoint(addr, cpu)
    return {"id": point, "at": describe(addr, c)}


@server.tool()
def watchpoint_add(address: str, size: int = 4, read: bool = False, write: bool = True, cpu: str = "9", condition: str = "", console: int = 0) -> dict:
    """Data watchpoint on [address, address+size). cpu: 9, 7 or both. The stop reports pc, value and size.
    condition: optional Lua expression over addr, val, pc."""
    c = con(console)
    addr = addr_of(address, c)
    cpu_arg = "both" if cpu == "both" else int(cpu)
    if condition:
        if not write or read:
            raise MelonError("conditional watchpoints support write=True, read=False")
        dom = "ARM7" if cpu_arg == 7 else "ARM9"
        out = c.lua_exec(f"print(event.onmemorywrite(function(addr, val, pc) return ({condition}) end, {addr}, 'mcp', '{dom}', {size}))")
        point = int(out.split()[-1])
    else:
        point = c.add_watchpoint(addr, size, read, write, cpu_arg)
    return {"id": point, "data": describe(addr, c)}


@server.tool()
def point_remove(point: int, console: int = 0) -> str:
    """Remove a breakpoint or watchpoint by id."""
    c = con(console)
    try:
        c.remove_point(point)
    except MelonError:
        c.lua_exec(f"event.unregisterbyid({point})")
    return "removed"


@server.tool()
def point_list(console: int = 0) -> list[dict]:
    """List breakpoints and watchpoints with hit counts."""
    c = con(console)
    out = []
    for p in c.points():
        p["start"] = describe(p["start"], c)
        p["end"] = f"{p['end']:08x}"
        out.append(p)
    return out


@server.tool()
def trace_start(capacity: int = 100000, cpu: str = "9", start: str = "0", end: str = "0xffffffff", console: int = 0) -> str:
    """Record executed instructions into a ring buffer (last `capacity` kept), optionally only PCs in [start, end]."""
    c = con(console)
    c.trace(capacity, "both" if cpu == "both" else int(cpu), addr_of(start, c), addr_of(end, c))
    return "tracing"


@server.tool()
def trace_read(limit: int = 200, functions_only: bool = False, console: int = 0) -> list[str]:
    """Read and clear the trace (oldest first). functions_only collapses it to symbol transitions."""
    c = con(console)
    out, last = [], None
    for cycle, cpu, pc, instr, cpsr in c.take_trace():
        if functions_only:
            name = describe(pc, c).split("+")[0]
            if name != last:
                out.append(f"{cycle} arm{cpu} {name}")
                last = name
        else:
            width = 4 if cpsr & 0x20 else 8
            out.append(f"{cycle} arm{cpu} {pc:08x} {instr & ((1 << (width * 4)) - 1):0{width}x} {describe(pc, c)}")
    return out[-limit:]


@server.tool()
def calls_track(enabled: bool = True, console: int = 0) -> str:
    """Track calls (BL/BLX) and exceptions: shadow call stack plus per-function call counts."""
    con(console).track_calls(enabled)
    return "on" if enabled else "off"


@server.tool()
def calls_read(cpu: int = 9, top: int = 100, console: int = 0) -> list[str]:
    """Function call counts since the last read (clears them). Use around an action to see which functions it runs."""
    c = con(console)
    counts = c.take_call_counts(cpu)
    rows = sorted(counts.items(), key=lambda kv: -kv[1])[:top]
    return [f"{n:8d}  {addr:08x}  {describe(addr, c)}" for addr, n in rows]


@server.tool()
def coverage_track(enabled: bool = True, console: int = 0) -> str:
    """Record every executed instruction address (cheap bitmap)."""
    con(console).coverage(enabled)
    return "on" if enabled else "off"


@server.tool()
def coverage_read(cpu: int = 9, console: int = 0) -> dict:
    """Executed code since the last read, grouped by symbol (clears it)."""
    c = con(console)
    addrs = c.take_coverage(cpu)
    funcs = {}
    for a in addrs:
        name = describe(a, c).split("+")[0]
        funcs[name] = funcs.get(name, 0) + 1
    return {"instructions": len(addrs), "functions": len(funcs),
            "by_function": dict(sorted(funcs.items(), key=lambda kv: -kv[1])[:500])}


@server.tool()
def writers_track(enabled: bool = True, console: int = 0) -> str:
    """Remember the PC of the last write to every main-RAM word."""
    con(console).track_writers(enabled)
    return "on" if enabled else "off"


@server.tool()
def last_writer(address: str, console: int = 0) -> str:
    """Which instruction last wrote this main-RAM word (needs writers_track)."""
    c = con(console)
    w = c.last_writer(addr_of(address, c))
    if not w:
        return "no write recorded"
    if w[0] == "dma":
        return f"arm{w[2]} DMA channel {w[1]}"
    pc, cpu = w
    return f"arm{cpu} {pc:08x} {describe(pc, c)}"


@server.tool()
def savestate_save(path: str, console: int = 0) -> str:
    """Save state at a frame boundary."""
    con(console).save_state(path)
    return f"saved {path}"


@server.tool()
def savestate_load(path: str, console: int = 0) -> str:
    """Load state at a frame boundary. A rejected file leaves the console unchanged."""
    con(console).load_state(path)
    return f"loaded {path}"


@server.tool()
def lua_run(source: str, is_file: bool = False, console: int = 0) -> str:
    """Start a Lua script (BizHawk-style API: memory, emu, joypad, event, savestate, debugger).
    The main body runs as a coroutine: emu.frameadvance() yields one frame. Replaces the previous script and its hooks."""
    c = con(console)
    c.lua_run(source, is_file)
    return c.lua_output() or "started"


@server.tool()
def lua_exec(code: str, console: int = 0) -> str:
    """Run a Lua chunk in the console's persistent Lua state (keeps hooks); returns printed output."""
    return con(console).lua_exec(code)


@server.tool()
def lua_output(console: int = 0) -> str:
    """Printed output and errors from Lua since the last read."""
    return con(console).lua_output()


@server.tool()
def lua_stop(console: int = 0) -> str:
    """Stop Lua and remove all Lua hooks (including conditional breakpoints)."""
    con(console).lua_stop()
    return "stopped"


@server.tool()
def packets_track(enabled: bool = True, capacity: int = 4096, console: int = 0) -> str:
    """Log local-wireless packets this console sends and receives."""
    con(console).packet_log(enabled, capacity)
    return "on" if enabled else "off"


@server.tool()
def packets_read(limit: int = 100, console: int = 0) -> list[str]:
    """Read and clear logged wireless packets."""
    kinds = {0: "data", 1: "cmd", 2: "reply", 3: "ack"}
    out = []
    for p in con(console).take_packets()[:limit]:
        d = "tx" if p["sent"] else "rx"
        out.append(f"{p['timestamp']} {d} {kinds.get(p['type'], p['type'])} {len(p['data'])}B {p['data'][:48].hex()}")
    return out


@server.tool()
def symbols_load(path: str, console: int = 0) -> dict:
    """Load symbols into one console's table: a dsd config.yaml (or its directory, e.g. config/usa/arm9) or an
    'addr name [size]' file. Overlay symbols resolve only while that overlay's code is in RAM.
    Tables are per console; a console id that does not exist yet gets its table when it is created."""
    return SYMBOLS.setdefault(console, SymbolTable()).load(path)


@server.tool()
def symbol_lookup(query: str, console: int = 0) -> dict:
    """Address to symbol, or symbol to address."""
    c = CONSOLES.get(console)
    t = syms(c) if c else SYMBOLS.get(console, NO_SYMBOLS)
    try:
        addr = int(query, 16)
        s = t.lookup(addr, c)
        return {"addr": f"{addr:08x}", "symbol": describe(addr, c), "size": s.size if s else None}
    except ValueError:
        s = t.resolve(query, c)
        if not s:
            raise MelonError(f"unknown symbol {query}")
        return {"symbol": s.name, "addr": f"{s.addr:08x}", "size": s.size, "module": s.module, "thumb": s.thumb}


@server.tool()
def overlays_loaded(console: int = 0) -> list[str]:
    """Overlays whose code is currently in RAM (needs symbols_load of a dsd config)."""
    c = con(console)
    t = syms(c)
    return [t.overlays[i].name for i in t.loaded_overlays(c)]


def main():
    native.lib().mds_set_local_dir(str(Path(os.environ.get("MELONDS_LOCAL_DIR", ".")).resolve()).encode())
    sym = os.environ.get("MELONDS_SYMBOLS", "")
    if rom := os.environ.get("MELONDS_ROM"):
        console_create(rom=rom, symbols=sym)  # the MELONDS_ROM console is console 0
    elif sym:
        symbols_load(sym, 0)
    server.run()
