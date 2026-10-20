import ctypes
import os
import sys
from ctypes import (POINTER, Structure, c_char, c_char_p, c_int, c_int16, c_int32, c_uint8, c_uint16, c_uint32,
                    c_uint64)
from dataclasses import dataclass
from pathlib import Path

IDLE, RUNNING, PARKED = 0, 1, 2
BREAK_KINDS = ["exec", "read", "write", "step", "request", "fault"]
KEYS = ["A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y"]
WATCH_READ, WATCH_WRITE = 1, 2


class MdsBreak(Structure):
    _fields_ = [("kind", c_int32), ("cpu", c_int32), ("size", c_int32), ("id", c_int32),
                ("pc", c_uint32), ("addr", c_uint32), ("value", c_uint32), ("dma", c_int32)]


class MdsPoint(Structure):
    _fields_ = [("id", c_int32), ("watch", c_int32), ("cpumask", c_uint32), ("flags", c_uint32),
                ("enabled", c_uint32), ("start", c_uint32), ("end", c_uint32), ("hits", c_uint64)]


class MdsTrace(Structure):
    _fields_ = [("cycle", c_uint64), ("pc", c_uint32), ("instr", c_uint32), ("cpsr", c_uint32), ("cpu", c_uint32)]


class MdsFrame(Structure):
    _fields_ = [("site", c_uint32), ("entry", c_uint32), ("ret", c_uint32), ("sp", c_uint32),
                ("mode", c_uint32), ("exception", c_uint32)]


class MdsPacket(Structure):
    _fields_ = [("timestamp", c_uint64), ("sent", c_uint32), ("type", c_uint32), ("length", c_uint32)]


class MdsTouchPoint(Structure):
    _fields_ = [("offset", c_uint32), ("x", c_uint16), ("y", c_uint16), ("down", c_uint8), ("pad", c_uint8 * 3)]


class MdsInput(Structure):
    _fields_ = [("seq", c_uint32), ("buttons", c_uint16), ("lid", c_uint8), ("touch_count", c_uint8),
                ("mic_kind", c_uint8), ("flags", c_uint8), ("mic_count", c_uint16), ("mic_level", c_uint16),
                ("pad2", c_uint16)]


class MdsInputLogEntry(Structure):
    _fields_ = [("frame", c_uint64), ("seq", c_uint32), ("buttons", c_uint16), ("lid", c_uint8),
                ("touch_count", c_uint8), ("mic_kind", c_uint8), ("pad", c_uint8), ("mic_level", c_uint16),
                ("pad2", c_uint16), ("pad3", c_uint16)]


INPUT_FLAG_CLIENT = 1
INPUT_MODES = {"repeat": 0, "neutral": 1, "wait": 2}


class MelonError(RuntimeError):
    pass


@dataclass
class Break:
    kind: str
    cpu: int
    pc: int
    addr: int
    value: int
    size: int
    id: int
    dma: int = -1


def _find_library():
    env = os.environ.get("MELONDS_HEADLESS_LIB")
    if env:
        return Path(env)
    root = Path(__file__).resolve().parents[3]
    for name in ("melonds_headless.dll", "melonds_headless.so", "melonds_headless.dylib"):
        for candidate in root.glob(f"build/*/{name}"):
            return candidate
    raise MelonError("melonds_headless library not found; set MELONDS_HEADLESS_LIB")


def _load():
    path = _find_library()
    if sys.platform == "win32":
        for extra in os.environ.get("MELONDS_DLL_DIRS", r"C:\msys64\ucrt64\bin").split(os.pathsep):
            if extra and os.path.isdir(extra):
                os.add_dll_directory(extra)
        os.add_dll_directory(str(path.parent))
    lib = ctypes.CDLL(str(path))
    sig = {
        "mds_version": (c_int, []),
        "mds_error": (c_char_p, []),
        "mds_set_local_dir": (None, [c_char_p]),
        "mds_set_log_level": (None, [c_int]),
        "mds_mp_timeout": (c_int, [c_int]),
        "mds_set_pacing": (None, [c_int]),
        "mds_create": (c_int, [c_char_p, c_char_p, c_char_p, c_char_p, c_int]),
        "mds_destroy": (None, [c_int]),
        "mds_load_rom": (c_int, [c_int, c_char_p, c_char_p]),
        "mds_reset": (c_int, [c_int]),
        "mds_run": (c_int, [POINTER(c_int), c_int, c_uint32, c_int, POINTER(c_int)]),
        "mds_wait": (c_int, [POINTER(c_int), c_int, c_int, POINTER(c_int)]),
        "mds_gdb": (c_int, [c_int, c_int, c_int, c_int]),
        "mds_start": (c_int, [c_int, c_uint32]),
        "mds_stop_after": (c_int, [c_int, c_uint32]),
        "mds_interrupt": (c_int, [c_int]),
        "mds_state": (c_int, [c_int]),
        "mds_take_break": (c_int, [c_int, POINTER(MdsBreak)]),
        "mds_frames": (c_uint64, [c_int]),
        "mds_screen": (c_int, [c_int, POINTER(c_uint32)]),
        "mds_set_keys": (c_int, [c_int, c_uint32]),
        "mds_touch": (c_int, [c_int, c_int, c_int]),
        "mds_set_lid": (c_int, [c_int, c_int]),
        "mds_input_submit": (c_int, [c_int, POINTER(MdsInput), POINTER(MdsTouchPoint), POINTER(c_int16)]),
        "mds_input_mode": (c_int, [c_int, c_int, c_int]),
        "mds_input_depth": (c_int, [c_int]),
        "mds_input_waiting": (c_int, [c_int]),
        "mds_input_clear": (c_int, [c_int]),
        "mds_input_log": (c_int, [c_int, c_uint64, POINTER(MdsInputLogEntry), c_int]),
        "mds_read": (c_int, [c_int, c_int, c_uint32, c_uint32, POINTER(c_uint8)]),
        "mds_write": (c_int, [c_int, c_int, c_uint32, c_uint32, POINTER(c_uint8)]),
        "mds_get_regs": (c_int, [c_int, c_int, POINTER(c_uint32)]),
        "mds_set_reg": (c_int, [c_int, c_int, c_int, c_uint32]),
        "mds_bp_add": (c_int, [c_int, c_uint32, c_uint32]),
        "mds_wp_add": (c_int, [c_int, c_uint32, c_uint32, c_uint32, c_uint32]),
        "mds_point_remove": (c_int, [c_int, c_int]),
        "mds_point_enable": (c_int, [c_int, c_int, c_int]),
        "mds_point_list": (c_int, [c_int, POINTER(MdsPoint), c_int]),
        "mds_step": (c_int, [c_int, c_int, c_uint32]),
        "mds_trace": (c_int, [c_int, c_uint32, c_uint32, c_uint32, c_uint32]),
        "mds_trace_take": (c_int, [c_int, POINTER(MdsTrace), c_int]),
        "mds_calls": (c_int, [c_int, c_int]),
        "mds_callstack": (c_int, [c_int, c_int, POINTER(MdsFrame), c_int]),
        "mds_callcounts_take": (c_int, [c_int, c_int, POINTER(c_uint32), POINTER(c_uint64), c_int]),
        "mds_coverage": (c_int, [c_int, c_int]),
        "mds_coverage_take": (c_int, [c_int, c_int, POINTER(c_uint32), c_int]),
        "mds_lastwriter": (c_int, [c_int, c_int]),
        "mds_lastwriter_get": (c_uint32, [c_int, c_uint32]),
        "mds_savestate": (c_int, [c_int, c_char_p]),
        "mds_loadstate": (c_int, [c_int, c_char_p]),
        "mds_lua_run": (c_int, [c_int, c_char_p, c_int]),
        "mds_lua_exec": (c_int, [c_int, c_char_p]),
        "mds_lua_stop": (c_int, [c_int]),
        "mds_lua_output": (c_int, [c_int, POINTER(c_char), c_int]),
        "mds_packets": (c_int, [c_int, c_int, c_int]),
        "mds_packets_take": (c_int, [c_int, POINTER(MdsPacket), POINTER(c_uint8), c_int, c_int]),
    }
    for name, (res, args) in sig.items():
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = args
    return lib


_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = _load()
    return _lib


def _check(ret):
    if ret < 0:
        raise MelonError(lib().mds_error().decode(errors="replace"))
    return ret


def _b(s):
    return s.encode() if s else None


def _cpu(cpu):
    if cpu in (7, 1, "arm7", "ARM7"):
        return 7
    if cpu in (9, 0, "arm9", "ARM9"):
        return 9
    raise MelonError(f"cpu must be 9 or 7, not {cpu!r}")


def _key_mask(keys):
    if isinstance(keys, int):
        return keys & 0xFFF
    mask = 0
    for k in keys:
        mask |= 1 << KEYS.index(k)
    return mask


def _mask(cpu):
    if cpu in ("both", 3):
        return 3
    return 2 if _cpu(cpu) == 7 else 1


def mp_timeout(ms=-1):
    return _check(lib().mds_mp_timeout(ms))


def set_pacing(fps):
    lib().mds_set_pacing(fps)


def run_group(consoles, frames, timeout_ms):
    ids = (c_int * len(consoles))(*[c.id for c in consoles])
    states = (c_int * len(consoles))()
    ret = _check(lib().mds_run(ids, len(consoles), frames, timeout_ms, states))
    return ret == 1, list(states)


def wait_group(consoles, timeout_ms):
    ids = (c_int * len(consoles))(*[c.id for c in consoles])
    states = (c_int * len(consoles))()
    ret = _check(lib().mds_wait(ids, len(consoles), timeout_ms, states))
    return ret == 1, list(states)


class Console:
    def __init__(self, bios9=None, bios7=None, firmware=None, nickname=None, debug=True):
        self.id = _check(lib().mds_create(_b(bios9), _b(bios7), _b(firmware), _b(nickname), int(debug)))
        self.rom = None

    def close(self):
        if self.id is not None:
            lib().mds_destroy(self.id)
            self.id = None

    def load_rom(self, path, save=None):
        _check(lib().mds_load_rom(self.id, _b(str(path)), _b(str(save) if save else None)))
        self.rom = str(path)

    def reset(self):
        _check(lib().mds_reset(self.id))

    def run(self, frames, timeout_ms=60000):
        return run_group([self], frames, timeout_ms)

    def wait(self, timeout_ms=60000):
        return wait_group([self], timeout_ms)

    def interrupt(self):
        _check(lib().mds_interrupt(self.id))

    def gdb(self, port9=3333, port7=3334, break_on_start=False):
        _check(lib().mds_gdb(self.id, port9, port7, int(break_on_start)))

    def start(self, frames=0xFFFFFFFF):
        _check(lib().mds_start(self.id, frames))

    def stop_after(self, frames=0):
        _check(lib().mds_stop_after(self.id, frames))

    @property
    def state(self):
        return _check(lib().mds_state(self.id))

    @property
    def frames(self):
        return lib().mds_frames(self.id)

    def take_break(self):
        b = MdsBreak()
        if _check(lib().mds_take_break(self.id, ctypes.byref(b))) == 0:
            return None
        return Break(BREAK_KINDS[b.kind], b.cpu, b.pc, b.addr, b.value, b.size, b.id, b.dma)

    def screen(self):
        buf = (c_uint32 * (256 * 384))()
        _check(lib().mds_screen(self.id, buf))
        return buf

    def set_keys(self, pressed):
        _check(lib().mds_set_keys(self.id, _key_mask(pressed)))

    def touch(self, x=None, y=None):
        _check(lib().mds_touch(self.id, -1 if x is None else x, -1 if y is None else y))

    def set_lid(self, closed):
        _check(lib().mds_set_lid(self.id, int(closed)))

    def submit_input(self, buttons=0, lid=False, touch=None, mic=None, mic_level=None, seq=0, client=False):
        """Queue one frame of input; returns the queue depth. buttons: key names or a bitmask.
        touch: list of (offset, x, y, down) or a single (x, y). mic: list of int16 samples.
        client=True marks the payload as coming from a socket client (subject to the stall timeout)."""
        if touch and not isinstance(touch[0], (tuple, list)):
            touch = [(0, touch[0], touch[1], 1)]
        pts = (MdsTouchPoint * max(len(touch or []), 1))()
        for i, (offset, x, y, down) in enumerate(touch or []):
            pts[i].offset, pts[i].x, pts[i].y, pts[i].down = offset, x, y, int(down)
        samples = list(mic or [])
        buf = (c_int16 * max(len(samples), 1))(*samples)
        hdr = MdsInput()
        hdr.seq = seq
        hdr.buttons = _key_mask(buttons)
        hdr.lid = int(lid)
        hdr.touch_count = len(touch or [])
        hdr.mic_kind = 2 if mic_level is not None else (1 if mic is not None else 0)
        hdr.flags = INPUT_FLAG_CLIENT if client else 0
        hdr.mic_count = len(samples)
        hdr.mic_level = mic_level or 0
        return _check(lib().mds_input_submit(self.id, ctypes.byref(hdr), pts, buf))

    def input_mode(self, mode=None, stall_frames=None):
        """mode: "repeat", "neutral" or "wait" (None = unchanged). stall_frames: repeat-mode timeout, 0 = off."""
        m = -1 if mode is None else INPUT_MODES[mode]
        _check(lib().mds_input_mode(self.id, m, -1 if stall_frames is None else stall_frames))

    def input_depth(self):
        return _check(lib().mds_input_depth(self.id))

    def input_waiting(self):
        return bool(_check(lib().mds_input_waiting(self.id)))

    def input_clear(self):
        _check(lib().mds_input_clear(self.id))

    def input_log(self, from_frame=0, max=4096):
        buf = (MdsInputLogEntry * max)()
        n = _check(lib().mds_input_log(self.id, from_frame, buf, max))
        return [{"frame": e.frame, "seq": e.seq, "buttons": e.buttons, "lid": e.lid, "touch_count": e.touch_count,
                 "mic_kind": e.mic_kind, "mic_level": e.mic_level} for e in buf[:n]]

    def read(self, addr, length, cpu=9):
        buf = (c_uint8 * length)()
        _check(lib().mds_read(self.id, _cpu(cpu), addr, length, buf))
        return bytes(buf)

    def write(self, addr, data, cpu=9):
        buf = (c_uint8 * len(data)).from_buffer_copy(data)
        _check(lib().mds_write(self.id, _cpu(cpu), addr, len(data), buf))

    def regs(self, cpu=9):
        out = (c_uint32 * 19)()
        _check(lib().mds_get_regs(self.id, _cpu(cpu), out))
        names = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "cpsr", "spsr", "r15_raw"]
        return dict(zip(names, out))

    def set_reg(self, reg, value, cpu=9):
        names = {f"r{i}": i for i in range(16)} | {"sp": 13, "lr": 14, "pc": 15}
        idx = names[reg] if isinstance(reg, str) else reg
        _check(lib().mds_set_reg(self.id, _cpu(cpu), idx, value))

    def add_breakpoint(self, addr, cpu=9):
        return _check(lib().mds_bp_add(self.id, _mask(cpu), addr))

    def add_watchpoint(self, addr, size, read=False, write=True, cpu=9):
        flags = (WATCH_READ if read else 0) | (WATCH_WRITE if write else 0)
        return _check(lib().mds_wp_add(self.id, _mask(cpu), addr, size, flags))

    def remove_point(self, point):
        _check(lib().mds_point_remove(self.id, point))

    def enable_point(self, point, enabled):
        _check(lib().mds_point_enable(self.id, point, int(enabled)))

    def points(self):
        out = (MdsPoint * 512)()
        n = _check(lib().mds_point_list(self.id, out, 512))
        return [{"id": p.id, "kind": "watch" if p.watch else "exec",
                 "cpu": {1: 9, 2: 7, 3: "both"}.get(p.cpumask, p.cpumask),
                 "read": bool(p.flags & 1), "write": bool(p.flags & 2), "enabled": bool(p.enabled),
                 "start": p.start, "end": p.end, "hits": p.hits} for p in out[:n]]

    def step(self, count=1, cpu=9):
        _check(lib().mds_step(self.id, _cpu(cpu), count))

    def trace(self, capacity, cpu="both", start=0, end=0xFFFFFFFF):
        _check(lib().mds_trace(self.id, _mask(cpu), start, end, capacity))

    def take_trace(self):
        out, buf = [], (MdsTrace * 8192)()
        while n := _check(lib().mds_trace_take(self.id, buf, 8192)):
            out.extend((t.cycle, t.cpu, t.pc, t.instr, t.cpsr) for t in buf[:n])
        return out

    def track_calls(self, enabled=True):
        _check(lib().mds_calls(self.id, int(enabled)))

    def callstack(self, cpu=9):
        out = (MdsFrame * 4096)()
        n = _check(lib().mds_callstack(self.id, _cpu(cpu), out, 4096))
        return [{"entry": f.entry, "site": f.site, "ret": f.ret, "sp": f.sp, "mode": f.mode,
                 "exception": bool(f.exception)} for f in out[:n]]

    def take_call_counts(self, cpu=9):
        counts = {}
        entries, hits = (c_uint32 * 8192)(), (c_uint64 * 8192)()
        while n := _check(lib().mds_callcounts_take(self.id, _cpu(cpu), entries, hits, 8192)):
            for i in range(n):
                counts[entries[i]] = counts.get(entries[i], 0) + hits[i]
        return counts

    def coverage(self, enabled=True):
        _check(lib().mds_coverage(self.id, int(enabled)))

    def take_coverage(self, cpu=9):
        out, buf = [], (c_uint32 * 65536)()
        while n := _check(lib().mds_coverage_take(self.id, _cpu(cpu), buf, 65536)):
            out.extend(buf[:n])
        return out

    def track_writers(self, enabled=True):
        _check(lib().mds_lastwriter(self.id, int(enabled)))

    def last_writer(self, addr):
        w = lib().mds_lastwriter_get(self.id, addr)
        if w == 0:
            return None
        if w >= 0xFFFFFFF0:
            return ("dma", (w >> 1) & 3, 7 if w & 1 else 9)
        return (w & ~1, 7 if w & 1 else 9)

    def save_state(self, path):
        _check(lib().mds_savestate(self.id, _b(str(path))))

    def load_state(self, path):
        _check(lib().mds_loadstate(self.id, _b(str(path))))

    def lua_run(self, source, is_file=False):
        _check(lib().mds_lua_run(self.id, _b(str(source)), int(is_file)))

    def lua_exec(self, code):
        _check(lib().mds_lua_exec(self.id, _b(code)))
        return self.lua_output()

    def lua_stop(self):
        _check(lib().mds_lua_stop(self.id))

    def lua_output(self):
        chunks, buf = [], (c_char * 65536)()
        while n := _check(lib().mds_lua_output(self.id, buf, 65536)):
            chunks.append(buf.raw[:n])
        return b"".join(chunks).decode(errors="replace")

    def packet_log(self, enabled=True, capacity=4096):
        _check(lib().mds_packets(self.id, int(enabled), capacity))

    def take_packets(self):
        out = []
        hdrs, data = (MdsPacket * 256)(), (c_uint8 * (256 * 2048))()
        while n := _check(lib().mds_packets_take(self.id, hdrs, data, 256, 256 * 2048)):
            off = 0
            for h in hdrs[:n]:
                out.append({"timestamp": h.timestamp, "sent": bool(h.sent), "type": h.type,
                            "data": bytes(data[off:off + h.length])})
                off += h.length
        return out
