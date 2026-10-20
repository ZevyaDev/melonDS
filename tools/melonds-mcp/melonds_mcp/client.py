"""Client library for the melonds_server socket protocol (version 1).

Pure Python (struct + socket). The encoders mirror src/frontend/headless/Protocol.h field for
field; that header is the specification. All integers are little-endian.

    c = Client(port)
    info = c.hello(empty_queue="wait")
    depth = c.input(buttons=["A"])
    c.live_start()
    ev = c.next_event("frame")

Replies and events are separated as they arrive. A request waits for its own reply (matched by
seq); anything else that arrives meanwhile is kept: events in ``events``, other replies for
their own ``result()`` call. Nothing runs in the background, so a client that stops calling
simply stops reading (the server then drops old frame events, see Protocol.h).
"""
import collections
import socket
import struct
import time
from dataclasses import dataclass, field

PROTOCOL_VERSION = 1
HEADER_SIZE = 8
MAX_BODY = 16 * 1024 * 1024
DEFAULT_CONSOLE = 0xFFFF
SCREENSHOT_BYTES = 256 * 384 * 4
KEYS = ["A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y"]
FRAME_CYCLES = 560190

ENC_ASCII, ENC_UTF16LE, ENC_UTF8, ENC_ALL = 1, 2, 4, 7
OPT_EMPTY_QUEUE, OPT_STALL_TIMEOUT, OPT_DEFAULT_CONSOLE = 1, 2, 3
EMPTY_QUEUE_MODES = {"repeat": 0, "neutral": 1, "wait": 2}
STATE_NAMES = {0: "idle", 1: "running", 2: "parked"}


class T:
    """Message types (MsgType)."""
    hello = 0x0001
    set_option = 0x0002
    console_create = 0x0101
    console_destroy = 0x0102
    console_list = 0x0103
    rom_load = 0x0104
    reset = 0x0105
    shutdown = 0x0106
    run = 0x0201
    live_start = 0x0202
    live_stop = 0x0203
    pause = 0x0204
    step = 0x0205
    input = 0x0301
    input_multi = 0x0302
    input_log_read = 0x0303
    read = 0x0401
    write = 0x0402
    registers = 0x0403
    set_register = 0x0404
    screenshot = 0x0405
    memory_search = 0x0501
    text_find = 0x0502
    subscribe = 0x0601
    unsubscribe = 0x0602
    breakpoint_add = 0x0701
    watchpoint_add = 0x0702
    point_remove = 0x0703
    point_list = 0x0704
    point_enable = 0x0705
    savestate_save = 0x0706
    savestate_load = 0x0707
    lua_run = 0x0708
    lua_exec = 0x0709
    lua_stop = 0x070A
    trace_config = 0x070B
    trace_read = 0x070C
    calls_track = 0x070D
    callstack = 0x070E
    call_counts_read = 0x070F
    coverage_track = 0x0710
    coverage_read = 0x0711
    writers_track = 0x0712
    last_writer = 0x0713
    packets_track = 0x0714
    packets_read = 0x0715
    symbols_load = 0x0716
    pacing = 0x0801
    mp_timeout = 0x0802
    ok = 0x8001
    error = 0x8002
    data = 0x8003
    frame = 0x9001
    break_ = 0x9002
    lua_output = 0x9003
    input_queue = 0x9004
    console_gone = 0x9005


EVENT_NAMES = {T.frame: "frame", T.break_: "break", T.lua_output: "lua_output",
               T.input_queue: "input_queue", T.console_gone: "console_gone"}

ERROR_CODES = {1: "bad_message", 2: "unknown_type", 3: "no_console", 4: "busy", 5: "console_running",
               6: "input_queue_full", 7: "bad_payload", 8: "bad_option", 9: "jit_on", 10: "not_supported",
               11: "internal", 12: "failed"}
BREAK_KINDS = ["exec", "read", "write", "step", "request", "fault"]


class ProtocolError(RuntimeError):
    pass


class ConnectionClosed(ProtocolError):
    pass


class ServerError(ProtocolError):
    """An error reply. ``name`` is the ErrorCode name (e.g. "no_console")."""

    def __init__(self, code, text, console=DEFAULT_CONSOLE):
        self.code = code
        self.name = ERROR_CODES.get(code, f"code{code}")
        self.text = text
        self.console = console
        super().__init__(f"{self.name}: {text}")


@dataclass
class Reply:
    type: int
    console: int
    body: bytes        # without the leading seq


@dataclass
class HelloInfo:
    version: int
    name: str
    pid: int
    port: int
    default_console: int
    ids: list


@dataclass
class ConsoleInfo:
    id: int
    state: str
    frames: int


@dataclass
class FrameEvent:
    console: int
    frame: int
    applied_seq: int
    input_applied: bool
    waiting_for_input: bool
    queue_depth: int
    dropped: int
    blocks: dict = field(default_factory=dict)     # sub id -> [bytes per range]
    kind = "frame"


@dataclass
class BreakEvent:
    console: int
    kind_id: int
    cpu: int
    size: int
    id: int
    pc: int
    addr: int
    value: int
    dma: int
    kind = "break"

    @property
    def what(self):
        return BREAK_KINDS[self.kind_id] if 0 <= self.kind_id < len(BREAK_KINDS) else str(self.kind_id)


@dataclass
class InputQueueEvent:
    console: int
    depth: int
    kind = "input_queue"


@dataclass
class ConsoleGoneEvent:
    console: int
    kind = "console_gone"


@dataclass
class LuaOutputEvent:
    console: int
    text: str
    kind = "lua_output"


def key_mask(keys):
    if isinstance(keys, int):
        return keys & 0xFFF
    mask = 0
    for k in keys:
        mask |= 1 << KEYS.index(k)
    return mask


# --------------------------------------------------------------------------- encoders

def _u8(v): return struct.pack("<B", v)
def _u16(v): return struct.pack("<H", v)
def _u32(v): return struct.pack("<I", v)
def _u64(v): return struct.pack("<Q", v)
def _str(s): b = s.encode("utf-8") if isinstance(s, str) else bytes(s); return _u32(len(b)) + b
def _bytes(b): return _u32(len(b)) + bytes(b)


def _ids(ids):
    ids = list(ids or ())
    return _u16(len(ids)) + b"".join(_u16(i) for i in ids)


def _cpu_mask(cpu):
    if cpu in ("both", 3):
        return 3
    return 2 if _cpu(cpu) == 7 else 1


def _cpu(cpu):
    if cpu in (7, 1, "arm7", "ARM7"):
        return 7
    if cpu in (9, 0, "arm9", "ARM9"):
        return 9
    raise ValueError(f"cpu must be 9 or 7, not {cpu!r}")


def encode_header(length, mtype, console):
    return struct.pack("<IHH", length, mtype, console)


def encode_input_payload(seq, buttons=0, lid=False, touch=None, mic=None, mic_level=None):
    """InputPayload: touch is a list of (offset, x, y, down) or one (x, y); mic a list of s16."""
    if touch and not isinstance(touch[0], (tuple, list)):
        touch = [(0, touch[0], touch[1], 1)]
    touch = list(touch or [])
    samples = list(mic or [])
    kind = 2 if mic_level is not None else (1 if mic is not None else 0)
    out = struct.pack("<IHBBBBHH", seq, key_mask(buttons), int(bool(lid)), len(touch), kind, 0,
                      len(samples), mic_level or 0)
    for offset, x, y, down in touch:
        out += struct.pack("<IHHB3x", offset, x, y, int(down))
    out += b"".join(struct.pack("<h", s) for s in samples)
    return out


# --------------------------------------------------------------------------- decoders

class _R:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, fmt):
        size = struct.calcsize(fmt)
        if self.p + size > len(self.d):
            raise ProtocolError("short reply")
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += size
        return v[0] if len(v) == 1 else v

    def string(self):
        n = self.take("<I")
        s = self.d[self.p:self.p + n]
        if len(s) != n:
            raise ProtocolError("short reply")
        self.p += n
        return s.decode("utf-8", "replace")

    def bytes(self, n):
        s = self.d[self.p:self.p + n]
        if len(s) != n:
            raise ProtocolError("short reply")
        self.p += n
        return s

    def rest(self):
        s = self.d[self.p:]
        self.p = len(self.d)
        return s


def decode_event(mtype, console, body):
    r = _R(body)
    r.take("<I")
    if mtype == T.frame:
        frame, applied, inp, waiting, depth, dropped, subs = r.take("<QIBBHIH")
        blocks = {}
        for _ in range(subs):
            sub_id, nranges = r.take("<IH")
            ranges = []
            for _ in range(nranges):
                ranges.append(r.bytes(r.take("<I")))
            blocks[sub_id] = ranges
        return FrameEvent(console, frame, applied, bool(inp), bool(waiting), depth, dropped, blocks)
    if mtype == T.break_:
        return BreakEvent(console, *r.take("<iiiiIIIi"))
    if mtype == T.input_queue:
        return InputQueueEvent(console, r.take("<H"))
    if mtype == T.console_gone:
        return ConsoleGoneEvent(console)
    if mtype == T.lua_output:
        return LuaOutputEvent(console, r.string())
    raise ProtocolError(f"unknown event {mtype:#x}")


def _names(r):
    """The optional names section of a with_symbols reply: u32 count + strings."""
    return [r.string() for _ in range(r.take("<I"))]


def _with_names(r, items):
    names = _names(r)
    if len(names) != len(items):
        raise ProtocolError("symbol names do not match the entries")
    return list(zip(items, names))


class Pending:
    """A request whose reply has not been collected yet (run and step reply when done)."""

    def __init__(self, client, seq, decode=None):
        self.client = client
        self.seq = seq
        self.decode = decode

    def ready(self):
        self.client.pump(0)
        return self.seq in self.client._replies or 0 in self.client._replies

    def result(self, timeout=None):
        reply = self.client._wait_reply(self.seq, timeout)
        return self.decode(reply) if self.decode else reply


class Client:
    def __init__(self, port, timeout=30.0, host="127.0.0.1"):
        self.timeout = timeout
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()
        self.seq = 0
        self.events = collections.deque()
        self._replies = {}
        self.closed = False
        self.last_console = DEFAULT_CONSOLE   # header console of the last reply

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ----------------------------------------------------------------- transport

    def _next_seq(self):
        self.seq = self.seq % 0xFFFFFFFF + 1
        return self.seq

    def send_raw(self, mtype, body, console=DEFAULT_CONSOLE):
        self.sock.settimeout(self.timeout)
        self.sock.sendall(encode_header(len(body), mtype, console) + body)

    def send(self, mtype, body=b"", console=DEFAULT_CONSOLE, seq=None):
        """Send a request; the u32 seq is prepended to body. Returns the seq."""
        seq = self._next_seq() if seq is None else seq
        self.send_raw(mtype, _u32(seq) + body, console)
        return seq

    def _parse(self):
        while len(self.buf) >= HEADER_SIZE:
            length, mtype, console = struct.unpack_from("<IHH", self.buf, 0)
            if length > MAX_BODY:
                raise ProtocolError("oversize message")
            if len(self.buf) < HEADER_SIZE + length:
                return
            body = bytes(self.buf[HEADER_SIZE:HEADER_SIZE + length])
            del self.buf[:HEADER_SIZE + length]
            if mtype in (T.ok, T.error, T.data):
                if length < 4:
                    raise ProtocolError("reply without seq")
                seq = struct.unpack_from("<I", body, 0)[0]
                self._replies[seq] = Reply(mtype, console, body[4:])
            else:
                self.events.append(decode_event(mtype, console, body))

    def pump(self, timeout=0.0):
        """Read what is available (waiting up to timeout seconds for the first bytes) and sort it.
        Returns True if anything was read."""
        got = False
        while True:
            self.sock.settimeout(timeout if not got else 0.0)
            try:
                chunk = self.sock.recv(1 << 20)
            except (socket.timeout, BlockingIOError):
                break
            except OSError as e:
                self._parse()
                raise ConnectionClosed(str(e))
            if not chunk:
                self._parse()
                raise ConnectionClosed("server closed the connection")
            self.buf += chunk
            got = True
            self._parse()
            timeout = 0.0
        return got

    def _wait_reply(self, seq, timeout=None):
        deadline = time.time() + (self.timeout if timeout is None else timeout)
        while True:
            self._parse()
            reply = self._replies.pop(seq, None)
            if reply is None and 0 in self._replies and seq != 0:
                reply = self._replies.pop(0)      # connection-level error (busy)
            if reply is not None:
                self.last_console = reply.console
                if reply.type == T.error:
                    r = _R(reply.body)
                    code = r.take("<H")
                    raise ServerError(code, r.string(), reply.console)
                return reply
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"no reply to seq {seq}")
            try:
                self.pump(min(left, 0.5))
            except ConnectionClosed:
                if seq in self._replies or 0 in self._replies:
                    continue
                raise

    def request(self, mtype, body=b"", console=DEFAULT_CONSOLE, timeout=None):
        return self._wait_reply(self.send(mtype, body, console), timeout)

    def request_async(self, mtype, body=b"", console=DEFAULT_CONSOLE, decode=None):
        return Pending(self, self.send(mtype, body, console), decode)

    # ----------------------------------------------------------------- events

    def next_event(self, kind=None, timeout=5.0, console=None):
        """Pop the oldest event (of the given kind, e.g. "frame"; others stay queued).
        Returns None on timeout."""
        deadline = time.time() + timeout
        while True:
            self._parse()
            for i, ev in enumerate(self.events):
                if (kind is None or ev.kind == kind) and (console is None or ev.console == console):
                    del self.events[i]
                    return ev
            left = deadline - time.time()
            if left <= 0:
                return None
            self.pump(min(left, 0.5))

    def take_events(self, kind=None):
        """Pop every queued event (of the kind) without waiting."""
        self.pump(0)
        out = [e for e in self.events if kind is None or e.kind == kind]
        self.events = collections.deque(e for e in self.events if not (kind is None or e.kind == kind))
        return out

    # ----------------------------------------------------------------- session

    def hello(self, default_console=DEFAULT_CONSOLE, empty_queue="repeat", stall_timeout=15,
              version=PROTOCOL_VERSION):
        mode = EMPTY_QUEUE_MODES[empty_queue] if isinstance(empty_queue, str) else empty_queue
        body = struct.pack("<IHBBH", version, default_console, mode, 0, stall_timeout)
        r = _R(self.request(T.hello, body).body)
        version = r.take("<I")
        name = r.string()
        pid, port, default, n = r.take("<IHHH")
        return HelloInfo(version, name, pid, port, default, [r.take("<H") for _ in range(n)])

    def set_option(self, key, value, console=DEFAULT_CONSOLE):
        self.request(T.set_option, struct.pack("<HHI", key, 0, value), console)

    def set_empty_queue(self, mode, console=DEFAULT_CONSOLE):
        self.set_option(OPT_EMPTY_QUEUE, EMPTY_QUEUE_MODES[mode] if isinstance(mode, str) else mode, console)

    def set_stall_timeout(self, frames, console=DEFAULT_CONSOLE):
        self.set_option(OPT_STALL_TIMEOUT, frames, console)

    def set_default_console(self, console_id):
        self.set_option(OPT_DEFAULT_CONSOLE, console_id)

    # ----------------------------------------------------------------- lifecycle

    def console_create(self, bios9="", bios7="", firmware="", nickname="", debug=True):
        body = _str(bios9) + _str(bios7) + _str(firmware) + _str(nickname) + _u8(int(debug))
        return _R(self.request(T.console_create, body).body).take("<H")

    def console_destroy(self, console):
        self.request(T.console_destroy, b"", console)

    def console_list(self):
        r = _R(self.request(T.console_list).body)
        out = []
        for _ in range(r.take("<H")):
            cid, state, frames = r.take("<HBQ")
            out.append(ConsoleInfo(cid, STATE_NAMES.get(state, str(state)), frames))
        return out

    def rom_load(self, rom, save="", console=DEFAULT_CONSOLE):
        self.request(T.rom_load, _str(str(rom)) + _str(str(save or "")), console)

    def reset(self, console=DEFAULT_CONSOLE):
        self.request(T.reset, b"", console)

    def shutdown(self):
        self.request(T.shutdown)

    # ----------------------------------------------------------------- execution

    def run(self, frames, consoles=(), wait=True, timeout=None):
        """Run the consoles (empty = all) for `frames`. The server replies when they have all stopped.
        wait=False returns a Pending."""
        body = _u32(frames) + _ids(consoles)
        if not wait:
            return self.request_async(T.run, body)
        return self.request(T.run, body, timeout=timeout)

    def live_start(self, fps=0, consoles=()):
        self.request(T.live_start, _u32(fps) + _ids(consoles))

    def live_stop(self, consoles=()):
        self.request(T.live_stop, _ids(consoles))

    def pause(self, console=DEFAULT_CONSOLE):
        self.request(T.pause, b"", console)

    def step(self, count=1, cpu=9, console=DEFAULT_CONSOLE, wait=True):
        body = _u8(_cpu(cpu)) + _u32(count)
        if not wait:
            return self.request_async(T.step, body, console)
        return self.request(T.step, body, console)

    # ----------------------------------------------------------------- input

    def input(self, buttons=0, lid=False, touch=None, mic=None, mic_level=None, console=DEFAULT_CONSOLE,
              seq=None):
        """Queue one frame of input. Returns the queue depth; the payload's seq (returned in
        ``last_seq``) comes back as FrameEvent.applied_seq when a frame applies it."""
        seq = self._next_seq() if seq is None else seq
        self.last_seq = seq
        payload = encode_input_payload(seq, buttons, lid, touch, mic, mic_level)
        reply = self._wait_reply(self.send_input_raw(payload, console))
        return _R(reply.body).take("<H")

    def send_input_raw(self, payload, console=DEFAULT_CONSOLE):
        """Send an already encoded InputPayload (starts with its seq); returns that seq."""
        self.send_raw(T.input, payload, console)
        return struct.unpack_from("<I", payload, 0)[0]

    # ----------------------------------------------------------------- memory

    def read(self, addr, length, cpu=9, console=DEFAULT_CONSOLE):
        return self.request(T.read, _u8(_cpu(cpu)) + _u32(addr) + _u32(length), console).body

    def write(self, addr, data, cpu=9, console=DEFAULT_CONSOLE):
        self.request(T.write, _u8(_cpu(cpu)) + _u32(addr) + _bytes(data), console)

    def registers(self, cpu=9, console=DEFAULT_CONSOLE):
        r = _R(self.request(T.registers, _u8(_cpu(cpu)), console).body)
        regs = [r.take("<I") for _ in range(r.take("<H"))]
        names = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "cpsr", "spsr", "r15_raw"]
        return dict(zip(names, regs))

    def set_register(self, reg, value, cpu=9, console=DEFAULT_CONSOLE):
        idx = {f"r{i}": i for i in range(16)} | {"sp": 13, "lr": 14, "pc": 15}
        idx = idx[reg] if isinstance(reg, str) else reg
        self.request(T.set_register, _u8(_cpu(cpu)) + _u8(idx) + _u32(value), console)

    def screenshot(self, console=DEFAULT_CONSOLE):
        """Returns (frame, BGRA bytes of 256x384, top screen above bottom)."""
        r = _R(self.request(T.screenshot, b"", console).body)
        frame = r.take("<Q")
        pixels = r.rest()
        if len(pixels) != SCREENSHOT_BYTES:
            raise ProtocolError("bad screenshot size")
        return frame, pixels

    # ----------------------------------------------------------------- search

    def memory_search(self, needle, size=0, start=0, length=0, limit=0, cpu=9, console=DEFAULT_CONSOLE,
                      with_symbols=False):
        """size 1/2/4: needle is an int, matched at aligned addresses; size 0: needle is bytes matched
        anywhere. start/length 0 = main RAM. Returns (addresses, truncated); with with_symbols each address
        becomes (address, symbol name)."""
        if size:
            needle = int(needle).to_bytes(size, "little")
        body = (_u8(_cpu(cpu)) + _u32(start) + _u32(length) + _u8(size) + _u32(limit) + _bytes(needle)
                + _u8(int(with_symbols)))
        r = _R(self.request(T.memory_search, body, console).body)
        truncated = bool(r.take("<B"))
        addrs = [r.take("<I") for _ in range(r.take("<I"))]
        return (_with_names(r, addrs) if with_symbols else addrs), truncated

    def text_find(self, text, encodings=0, start=0, length=0, limit=0, cpu=9, console=DEFAULT_CONSOLE,
                  with_symbols=False):
        """Returns ([(address, encoding)], truncated); encoding is ENC_ASCII, ENC_UTF16LE or ENC_UTF8.
        With with_symbols each hit becomes ((address, encoding), symbol name)."""
        body = (_u8(_cpu(cpu)) + _u32(start) + _u32(length) + _str(text) + _u8(encodings) + _u32(limit)
                + _u8(int(with_symbols)))
        r = _R(self.request(T.text_find, body, console).body)
        truncated = bool(r.take("<B"))
        hits = [r.take("<IB") for _ in range(r.take("<I"))]
        return (_with_names(r, hits) if with_symbols else hits), truncated

    # ----------------------------------------------------------------- subscriptions

    def subscribe(self, sub_id, ranges=(), every=1, console=DEFAULT_CONSOLE):
        """ranges: [(cpu, addr, len)]. With no ranges the console gets plain per-frame events."""
        body = _u32(sub_id) + _u32(every) + _u16(len(ranges))
        for cpu, addr, length in ranges:
            body += _u8(_cpu(cpu)) + _u32(addr) + _u32(length)
        self.request(T.subscribe, body, console)

    def unsubscribe(self, sub_id, console=DEFAULT_CONSOLE):
        self.request(T.unsubscribe, _u32(sub_id), console)

    # ----------------------------------------------------------------- input log

    def input_log_read(self, from_frame=0, max=0, console=DEFAULT_CONSOLE):
        """The applied-input log from from_frame. Returns (entries, truncated); each entry is a dict with
        frame, seq, buttons, lid, touch [(offset, x, y, down)], mic_kind, mic [s16], mic_level."""
        r = _R(self.request(T.input_log_read, _u64(from_frame) + _u32(max), console).body)
        truncated = bool(r.take("<B"))
        entries = []
        for _ in range(r.take("<I")):
            frame = r.take("<Q")
            seq, buttons, lid, ntouch, kind, _res, nmic, level = r.take("<IHBBBBHH")
            touch = [r.take("<IHHB3x") for _ in range(ntouch)]
            mic = [r.take("<h") for _ in range(nmic)]
            entries.append({"frame": frame, "seq": seq, "buttons": buttons, "lid": lid, "touch": touch,
                            "mic_kind": kind, "mic": mic, "mic_level": level})
        return entries, truncated

    # ----------------------------------------------------------------- points

    def breakpoint_add(self, addr, cpu=9, console=DEFAULT_CONSOLE):
        """Returns the point id. cpu: 9, 7 or "both"."""
        r = self.request(T.breakpoint_add, _u32(_cpu_mask(cpu)) + _u32(addr), console)
        return _R(r.body).take("<i")

    def watchpoint_add(self, addr, size, read=False, write=True, cpu=9, console=DEFAULT_CONSOLE):
        flags = (1 if read else 0) | (2 if write else 0)
        body = _u32(_cpu_mask(cpu)) + _u32(addr) + _u32(size) + _u32(flags)
        return _R(self.request(T.watchpoint_add, body, console).body).take("<i")

    def point_remove(self, point, console=DEFAULT_CONSOLE):
        self.request(T.point_remove, struct.pack("<i", point), console)

    def point_enable(self, point, enabled, console=DEFAULT_CONSOLE):
        self.request(T.point_enable, struct.pack("<iB", point, int(enabled)), console)

    def point_list(self, console=DEFAULT_CONSOLE):
        r = _R(self.request(T.point_list, b"", console).body)
        out = []
        for _ in range(r.take("<I")):
            pid, watch, enabled, mask, flags, start, end, hits = r.take("<iBBIIIIQ")
            out.append({"id": pid, "kind": "watch" if watch else "exec", "enabled": bool(enabled),
                        "cpu": {1: 9, 2: 7, 3: "both"}.get(mask, mask), "read": bool(flags & 1),
                        "write": bool(flags & 2), "start": start, "end": end, "hits": hits})
        return out

    # ----------------------------------------------------------------- savestates, Lua

    def savestate_save(self, path, console=DEFAULT_CONSOLE):
        self.request(T.savestate_save, _str(str(path)), console)

    def savestate_load(self, path, console=DEFAULT_CONSOLE):
        """Loads the state; the console's input queue is cleared and an input_queue(0) event follows."""
        self.request(T.savestate_load, _str(str(path)), console)

    def lua_run(self, source, is_file=False, console=DEFAULT_CONSOLE):
        self.request(T.lua_run, _u8(int(is_file)) + _str(str(source)), console)

    def lua_exec(self, code, console=DEFAULT_CONSOLE):
        self.request(T.lua_exec, _str(code), console)

    def lua_stop(self, console=DEFAULT_CONSOLE):
        self.request(T.lua_stop, b"", console)

    def lua_text(self, console=None, timeout=0.2):
        """Join the lua_output events queued so far (for one console, or all)."""
        self.pump(timeout)
        return "".join(e.text for e in self.take_events("lua_output") if console is None or e.console == console)

    # ----------------------------------------------------------------- analysis

    def trace_config(self, capacity, cpu="both", start=0, end=0xFFFFFFFF, console=DEFAULT_CONSOLE):
        """capacity 0 turns the trace off."""
        self.request(T.trace_config, _u32(_cpu_mask(cpu)) + _u32(start) + _u32(end) + _u32(capacity), console)

    def trace_read(self, max=0, with_symbols=False, console=DEFAULT_CONSOLE):
        """Clearing. Returns (entries, truncated), oldest first; each entry is a dict with cycle, cpu, pc,
        opcode, cpsr (and name with with_symbols). When truncated, the rest stays for the next read."""
        r = _R(self.request(T.trace_read, _u32(max) + _u8(int(with_symbols)), console).body)
        truncated = bool(r.take("<B"))
        entries = []
        for _ in range(r.take("<I")):
            cycle, cpu, pc, opcode, cpsr = r.take("<QBIII")
            entries.append({"cycle": cycle, "cpu": cpu, "pc": pc, "opcode": opcode, "cpsr": cpsr})
        if with_symbols:
            for e, n in zip(entries, _names(r)):
                e["name"] = n
        return entries, truncated

    def calls_track(self, enabled=True, console=DEFAULT_CONSOLE):
        self.request(T.calls_track, _u8(int(enabled)), console)

    def callstack(self, cpu=9, with_symbols=False, console=DEFAULT_CONSOLE):
        """Frames, innermost first: dicts with site, entry, ret, sp, mode, exception (and name, site_name)."""
        r = _R(self.request(T.callstack, _u8(_cpu(cpu)) + _u8(int(with_symbols)), console).body)
        frames = []
        for _ in range(r.take("<I")):
            site, entry, ret, sp, mode, exc = r.take("<IIIIIB")
            frames.append({"site": site, "entry": entry, "ret": ret, "sp": sp, "mode": mode,
                           "exception": bool(exc)})
        if with_symbols:
            for f, n in zip(frames, _names(r)):
                f["name"] = n
            for f, n in zip(frames, _names(r)):
                f["site_name"] = n
        return frames

    def call_counts_read(self, cpu=9, max=0, with_symbols=False, console=DEFAULT_CONSOLE):
        """Clearing. Returns (list of {entry, count[, name]}, truncated)."""
        body = _u8(_cpu(cpu)) + _u32(max) + _u8(int(with_symbols))
        r = _R(self.request(T.call_counts_read, body, console).body)
        truncated = bool(r.take("<B"))
        out = []
        for _ in range(r.take("<I")):
            entry, count = r.take("<IQ")
            out.append({"entry": entry, "count": count})
        if with_symbols:
            for e, n in zip(out, _names(r)):
                e["name"] = n
        return out, truncated

    def coverage_track(self, enabled=True, console=DEFAULT_CONSOLE):
        self.request(T.coverage_track, _u8(int(enabled)), console)

    def coverage_read(self, cpu=9, max=0, with_symbols=False, console=DEFAULT_CONSOLE):
        """Clearing. Returns (addresses, truncated); with with_symbols each becomes (address, name)."""
        body = _u8(_cpu(cpu)) + _u32(max) + _u8(int(with_symbols))
        r = _R(self.request(T.coverage_read, body, console).body)
        truncated = bool(r.take("<B"))
        addrs = [r.take("<I") for _ in range(r.take("<I"))]
        return (_with_names(r, addrs) if with_symbols else addrs), truncated

    def writers_track(self, enabled=True, console=DEFAULT_CONSOLE):
        self.request(T.writers_track, _u8(int(enabled)), console)

    def last_writer(self, addr, console=DEFAULT_CONSOLE):
        """None, {"kind": "cpu", "cpu", "pc"} or {"kind": "dma", "cpu", "channel"}."""
        r = _R(self.request(T.last_writer, _u32(addr), console).body)
        kind, cpu, channel, _res, pc, raw = r.take("<BBBBII")
        if kind == 0:
            return None
        if kind == 1:
            return {"kind": "cpu", "cpu": cpu, "pc": pc, "raw": raw}
        return {"kind": "dma", "cpu": cpu, "channel": channel, "raw": raw}

    def packets_track(self, enabled=True, capacity=4096, console=DEFAULT_CONSOLE):
        self.request(T.packets_track, _u8(int(enabled)) + _u32(capacity), console)

    def packets_read(self, max=0, console=DEFAULT_CONSOLE):
        """Clearing; works while running. Returns (list of {timestamp, sent, type, data}, truncated)."""
        r = _R(self.request(T.packets_read, _u32(max), console).body)
        truncated = bool(r.take("<B"))
        out = []
        for _ in range(r.take("<I")):
            ts, sent, ptype = r.take("<QBB")
            out.append({"timestamp": ts, "sent": bool(sent), "type": ptype, "data": r.bytes(r.take("<I"))})
        return out, truncated

    def symbols_load(self, path, console=DEFAULT_CONSOLE):
        """Load a symbol file into the console's table ("" clears it). Returns (symbols, modules)."""
        r = _R(self.request(T.symbols_load, _str(str(path)), console).body)
        return r.take("<I"), r.take("<I")

    # ----------------------------------------------------------------- process-wide

    def pacing(self, fps):
        """Frame pacing for every console: fps frames per second, 0 = unlimited."""
        self.request(T.pacing, _u32(fps))

    def mp_timeout(self, ms):
        """Wireless receive timeout in milliseconds (all consoles)."""
        self.request(T.mp_timeout, _u32(ms))
