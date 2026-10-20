"""Commands about memory, registers, symbols and per-game watch lists."""
import os
import re
import struct
from pathlib import Path

from ..client import ENC_ASCII, ENC_UTF8, ENC_UTF16LE
from ..ramtools import WATCH_TYPES, Watches
from .common import CliError, Session, home_dir, int_arg

ENC_NAMES = {ENC_ASCII: "ascii", ENC_UTF16LE: "utf16le", ENC_UTF8: "utf8"}
ENC_BITS = {"ascii": ENC_ASCII, "utf16": ENC_UTF16LE, "utf16le": ENC_UTF16LE, "utf8": ENC_UTF8}
VIEW_TYPES = sorted(WATCH_TYPES) + ["ascii", "utf16"]
HEADER_GAME_CODE = 0x027FFE0C        # cart header copy in main RAM (header offset 0x0C)


def decode_values(kind, raw):
    if kind in WATCH_TYPES:
        size, fmt = WATCH_TYPES[kind]
        count = len(raw) // size
        vals = list(struct.unpack("<" + fmt[1] * count, raw[:count * size]))
        return [round(v / 4096, 4) for v in vals] if kind.startswith("fx") else vals
    if kind == "ascii":
        return [raw.split(b"\0")[0].decode("latin-1")]
    return [raw.decode("utf-16-le", errors="replace").split("\0")[0]]


# ----------------------------------------------------------------------------- read / write

def cmd_read(args):
    with Session(args) as s:
        addr = s.addr(args.addr)
        raw = s.cl.read(addr, args.length, args.cpu, s.console)
        out = {"console": s.console, "cpu": args.cpu, "addr": addr, "length": len(raw)}
        name = s.describe(addr)
        if name:
            out["symbol"] = name
        if args.out:
            Path(args.out).write_bytes(raw)
            out["out"] = args.out
        else:
            out["hex"] = raw.hex()
        if args.as_type:
            out["type"] = args.as_type
            out["values"] = decode_values(args.as_type, raw)
        return out


def cmd_write(args):
    with Session(args) as s:
        addr = s.addr(args.addr)
        if args.hex is not None:
            data = bytes.fromhex(args.hex.replace(" ", ""))
        elif args.file:
            data = Path(args.file).read_bytes()
        elif args.value is not None:
            data = (s.addr(args.value) & ((1 << (8 * args.size)) - 1)).to_bytes(args.size, "little")
        else:
            raise CliError("usage", "write needs VALUE, --hex or --file")
        s.cl.write(addr, data, args.cpu, s.console)
        return {"ok": True, "addr": addr, "length": len(data)}


def cmd_search(args):
    with Session(args) as s:
        start = s.addr(args.start) if args.start is not None else 0
        if args.bytes is not None:
            needle, size = bytes.fromhex(args.bytes.replace(" ", "")), 0
        elif args.value is not None:
            needle, size = s.addr(args.value), args.size
        else:
            raise CliError("usage", "search needs VALUE or --bytes")
        hits, truncated = s.cl.memory_search(needle, size, start, args.length, args.limit, args.cpu, s.console,
                                             args.with_symbols)
        if args.with_symbols:
            return {"addresses": [a for a, _ in hits], "names": [n for _, n in hits], "truncated": truncated}
        return {"addresses": hits, "truncated": truncated}


def cmd_find_text(args):
    with Session(args) as s:
        mask = 0
        for name in (args.enc or "").split(","):
            if name.strip():
                if name.strip() not in ENC_BITS:
                    raise CliError("usage", f"unknown encoding {name!r} (ascii, utf16, utf8)")
                mask |= ENC_BITS[name.strip()]
        start = s.addr(args.start) if args.start is not None else 0
        hits, truncated = s.cl.text_find(args.text, mask, start, args.length, args.limit, args.cpu, s.console,
                                         args.with_symbols)
        out = []
        for h in hits:
            (a, enc), name = h if args.with_symbols else (h, None)
            d = {"addr": a, "encoding": ENC_NAMES.get(enc, str(enc))}
            if name is not None:
                d["name"] = name
            out.append(d)
        return {"hits": out, "truncated": truncated}


def cmd_regs(args):
    with Session(args) as s:
        regs = s.cl.registers(args.cpu, s.console)
        out = {"console": s.console, "cpu": args.cpu, "regs": regs}
        name = s.describe(regs["pc"])
        if name:
            out["pc_symbol"] = name
        return out


def cmd_set_reg(args):
    with Session(args) as s:
        value = s.addr(args.value)
        s.cl.set_register(args.reg, value, args.cpu, s.console)
        return {"ok": True, "reg": args.reg, "value": value}


# ----------------------------------------------------------------------------- symbols

def cmd_symbols_load(args):
    with Session(args) as s:
        path = os.path.abspath(args.path)
        symbols, modules = s.cl.symbols_load(path, s.console)
        s.remember_symbols(path)
        local = None
        try:
            local = s.symbols.summary()
        except Exception:
            pass
        return {"server": {"symbols": symbols, "modules": modules}, "symbols": symbols, "modules": modules,
                "path": path, "client": local}


def cmd_symbols_clear(args):
    with Session(args) as s:
        s.cl.symbols_load("", s.console)
        s.remember_symbols(None)
        return {"ok": True}


def cmd_symbols_lookup(args):
    with Session(args) as s:
        if s.symbols is None:
            raise CliError("no_symbols", "no symbol file is known (use --symbols FILE or `symbols load FILE`)")
        addr = s.addr(args.what)
        sym = s.symbols.lookup(addr, s.view)
        out = {"addr": addr, "name": s.describe(addr)}
        if sym:
            out.update({"symbol": sym.name, "module": sym.module, "offset": (addr & ~1) - sym.addr})
        return out


# ----------------------------------------------------------------------------- watches

def game_key(s):
    try:
        raw = s.cl.read(HEADER_GAME_CODE, 4, 9, s.console)
    except Exception:
        return "nocart"
    if re.fullmatch(rb"[A-Za-z0-9]{4}", raw):
        return raw.decode()
    return "nocart"


def watch_path(game):
    return home_dir() / "watches" / f"{game}.json"


def load_watches(game):
    w = Watches()
    path = watch_path(game)
    if path.exists():
        w.load(path)
    return w


def save_watches(game, w):
    path = watch_path(game)
    path.parent.mkdir(parents=True, exist_ok=True)
    w.save(path)


def cmd_watch_add(args):
    with Session(args) as s:
        game = args.game or game_key(s)
        w = load_watches(game)
        w.add(args.name, s.addr(args.addr), args.type, args.cpu, args.length, args.note or "")
        save_watches(game, w)
        return {"ok": True, "game": game, "watch": {"name": args.name, **w.items[args.name]}}


def cmd_watch_remove(args):
    with Session(args) as s:
        game = args.game or game_key(s)
        w = load_watches(game)
        if args.name not in w.items:
            raise CliError("unknown_watch", f"no watch named {args.name!r} for game {game}")
        w.remove(args.name)
        save_watches(game, w)
        return {"ok": True, "game": game}


def cmd_watch_list(args):
    with Session(args) as s:
        game = args.game or game_key(s)
        w = load_watches(game)
        return {"game": game, "watches": [{"name": n, **v} for n, v in w.items.items()]}


def cmd_watch_read(args):
    with Session(args) as s:
        game = args.game or game_key(s)
        w = load_watches(game)
        names = args.names or list(w.items)
        for n in names:
            if n not in w.items:
                raise CliError("unknown_watch", f"no watch named {n!r} for game {game}")
        return {"game": game, "values": {n: w.value(s.view, w.items[n]) for n in names}}


# ----------------------------------------------------------------------------- registration

def register(sub, leaf):
    def cpu(p):
        p.add_argument("--cpu", type=int, choices=(9, 7), default=9)

    p = leaf(sub, "read", "read memory (ADDR may be a symbol name[+off])", cmd_read)
    p.add_argument("addr")
    p.add_argument("length", type=int_arg)
    cpu(p)
    p.add_argument("--as", dest="as_type", choices=VIEW_TYPES, help="also decode as this type")
    p.add_argument("--out", help="write the raw bytes to this file instead of printing hex")

    p = leaf(sub, "write", "write VALUE (--size bytes), --hex bytes or --file contents", cmd_write)
    p.add_argument("addr")
    p.add_argument("value", nargs="?")
    p.add_argument("--size", type=int, choices=(1, 2, 4), default=4)
    p.add_argument("--hex")
    p.add_argument("--file")
    cpu(p)

    p = leaf(sub, "search", "scan memory in the server for a value or byte string", cmd_search)
    p.add_argument("value", nargs="?")
    p.add_argument("--size", type=int, choices=(1, 2, 4), default=4)
    p.add_argument("--bytes", help="hex byte string, matched at any offset")
    p.add_argument("--start")
    p.add_argument("--length", type=int_arg, default=0, help="0 = main RAM")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--with-symbols", action="store_true")
    cpu(p)

    p = leaf(sub, "find-text", "find a string (ascii/utf16/utf8) in memory", cmd_find_text)
    p.add_argument("text")
    p.add_argument("--enc", help="comma list of ascii,utf16,utf8 (default all)")
    p.add_argument("--start")
    p.add_argument("--length", type=int_arg, default=0)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--with-symbols", action="store_true")
    cpu(p)

    p = leaf(sub, "regs", "print the registers", cmd_regs)
    cpu(p)
    p = leaf(sub, "set-reg", "set a register (r0-r15, sp, lr, pc)", cmd_set_reg)
    p.add_argument("reg")
    p.add_argument("value")
    cpu(p)

    sym = sub.add_parser("symbols", help="symbol tables").add_subparsers(dest="sub", required=True)
    p = leaf(sym, "load", "load a symbol file on the server and remember it for name lookups", cmd_symbols_load)
    p.add_argument("path")
    leaf(sym, "clear", "drop the console's symbols", cmd_symbols_clear)
    p = leaf(sym, "lookup", "name for an address, or address for a name (client side)", cmd_symbols_lookup)
    p.add_argument("what")

    w = sub.add_parser("watch", help="per-game watch lists").add_subparsers(dest="sub", required=True)

    def game(p):
        p.add_argument("--game", help="game key (default: game code from the ROM header, else nocart)")

    p = leaf(w, "add", "add or replace a watch", cmd_watch_add)
    p.add_argument("name")
    p.add_argument("addr")
    p.add_argument("--type", choices=VIEW_TYPES + ["bytes"], default="u32")
    p.add_argument("--length", type=int, default=0, help="bytes, for ascii/utf16/bytes")
    p.add_argument("--note", default="")
    cpu(p)
    game(p)
    p = leaf(w, "remove", "remove a watch", cmd_watch_remove)
    p.add_argument("name")
    game(p)
    game(leaf(w, "list", "list the game's watches", cmd_watch_list))
    p = leaf(w, "read", "read watch values (all, or the named ones)", cmd_watch_read)
    p.add_argument("names", nargs="*")
    game(p)
