import json
import re
import struct
from pathlib import Path

np = None  # imported on first use: the CLI does not need numpy

DTYPES = {
    (1, False): "<u1", (1, True): "<i1",
    (2, False): "<u2", (2, True): "<i2",
    (4, False): "<u4", (4, True): "<i4",
}

COMPARES = ("changed", "unchanged", "increased", "decreased", "eq", "ne", "gt", "lt", "ge", "le", "delta")


class RamSearch:
    def __init__(self, console, start=0x02000000, length=0x400000, size=4, signed=True, cpu=9):
        global np
        import numpy as np
        if (size, signed) not in DTYPES:
            raise ValueError("size must be 1, 2 or 4")
        self.console = console
        self.start = start
        self.length = length - length % size
        self.size = size
        self.dtype = np.dtype(DTYPES[(size, signed)])
        self.cpu = cpu
        self.prev = self._read()
        self.cand = np.arange(len(self.prev))
        self.history = []

    def _read(self):
        return np.frombuffer(self.console.read(self.start, self.length, self.cpu), dtype=self.dtype)

    def filter(self, compare, value=None):
        if compare not in COMPARES:
            raise ValueError(f"compare must be one of {', '.join(COMPARES)}")
        cur = self._read()
        c = cur[self.cand].astype(np.int64)
        p = self.prev[self.cand].astype(np.int64)
        ref = p if value is None or compare == "delta" else np.int64(value)
        mask = {
            "changed": c != p,
            "unchanged": c == p,
            "increased": c > p,
            "decreased": c < p,
            "eq": c == ref,
            "ne": c != ref,
            "gt": c > ref,
            "lt": c < ref,
            "ge": c >= ref,
            "le": c <= ref,
            "delta": (c - p) == (value or 0),
        }[compare]
        self.history.append((compare, value, len(self.cand)))
        self.cand = self.cand[mask]
        self.before = self.prev
        self.prev = cur
        return len(self.cand)

    def rows(self, limit=50):
        cur = self._read()
        before = getattr(self, "before", self.prev)
        out = []
        for i in self.cand[:limit]:
            out.append((self.start + int(i) * self.size, int(cur[i]), int(before[i])))
        return out

    def addresses(self):
        return [self.start + int(i) * self.size for i in self.cand]


WATCH_TYPES = {
    "u8": (1, "<B"), "s8": (1, "<b"), "u16": (2, "<H"), "s16": (2, "<h"),
    "u32": (4, "<I"), "s32": (4, "<i"), "fx32": (4, "<i"), "fx16": (2, "<h"),
}


class Watches:
    def __init__(self):
        self.items = {}

    def add(self, name, addr, kind="u32", cpu=9, length=0, note=""):
        if kind not in WATCH_TYPES and kind not in ("bytes", "ascii", "utf16"):
            raise ValueError(f"unknown watch type {kind}")
        self.items[name] = {"addr": addr, "type": kind, "cpu": cpu, "length": length, "note": note}

    def remove(self, name):
        self.items.pop(name, None)

    def value(self, console, w):
        kind = w["type"]
        if kind in WATCH_TYPES:
            size, fmt = WATCH_TYPES[kind]
            v = struct.unpack(fmt, console.read(w["addr"], size, w["cpu"]))[0]
            if kind == "fx32" or kind == "fx16":
                return round(v / 4096, 4)
            return v
        raw = console.read(w["addr"], w["length"] or 32, w["cpu"])
        if kind == "bytes":
            return raw.hex()
        if kind == "ascii":
            return raw.split(b"\0")[0].decode("latin-1")
        return raw.decode("utf-16-le", errors="replace").split("\0")[0]

    def read(self, console):
        return {name: self.value(console, w) for name, w in self.items.items()}

    def save(self, path):
        Path(path).write_text(json.dumps(self.items, indent=2))

    def load(self, path):
        self.items.update(json.loads(Path(path).read_text()))
        return list(self.items)


def decode_values(kind, raw):
    """Decode little-endian bytes as an array of one watch type (u8 ... fx16; fx values are /4096), or as one
    ascii / utf16 string (up to the first NUL)."""
    if kind in WATCH_TYPES:
        size, fmt = WATCH_TYPES[kind]
        count = len(raw) // size
        vals = list(struct.unpack("<" + fmt[1] * count, raw[:count * size]))
        return [round(v / 4096, 4) for v in vals] if kind.startswith("fx") else vals
    if kind == "ascii":
        return [raw.split(b"\0")[0].decode("latin-1")]
    return [raw.decode("utf-16-le", errors="replace").split("\0")[0]]


TEXT_ENCODINGS = {"ascii": "ascii", "utf16": "utf-16-le", "utf16le": "utf-16-le", "utf-16-le": "utf-16-le",
                  "utf8": "utf-8", "utf-8": "utf-8"}


def text_search(console, text, start=0x02000000, length=0x400000, limit=32, encodings=None, cpu=9):
    """(addr, encoding) of each hit of `text` as ASCII, UTF-16LE and UTF-8, or only as `encodings` (names
    from TEXT_ENCODINGS); at most `limit` hits."""
    if encodings:
        for e in encodings:
            if e.lower() not in TEXT_ENCODINGS:
                raise ValueError(f"unknown encoding {e!r} (ascii, utf16, utf8)")
        encs = list(dict.fromkeys(TEXT_ENCODINGS[e.lower()] for e in encodings))
    else:
        encs = ["ascii", "utf-16-le", "utf-8"]
    data = console.read(start, length, cpu)
    hits = []
    for enc in encs:
        try:
            needle = text.encode(enc)
        except UnicodeEncodeError:
            continue
        for m in re.finditer(re.escape(needle), data):
            hits.append((start + m.start(), enc))
            if len(hits) >= limit:
                return hits
    return hits


def parse_sequence(spec):
    steps = []
    for token in spec.replace(",", " ").split():
        if token.lower().startswith("w"):
            steps.append(([], int(token[1:])))
            continue
        keys, _, frames = token.partition("*")
        steps.append((keys.split("+"), int(frames or 4)))
    return steps
