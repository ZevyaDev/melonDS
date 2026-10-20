"""Commands for points, trace, calls, coverage, last writer, packets and Lua."""
import os
import time

from .common import CliError, Session, break_dict, int_arg


def onoff(text):
    if text.lower() in ("on", "1", "true", "yes"):
        return True
    if text.lower() in ("off", "0", "false", "no"):
        return False
    raise CliError("usage", f"expected on or off, not {text!r}")


def cpu_arg(text):
    if text == "both":
        return "both"
    if text in ("9", "7"):
        return int(text)
    raise ValueError(f"cpu must be 9, 7 or both, not {text!r}")


# ----------------------------------------------------------------------------- points

def cmd_points_list(args):
    with Session(args) as s:
        points = s.cl.point_list(s.console)
        for p in points:
            name = s.describe(p["start"])
            if name:
                p["symbol"] = name
        return {"points": points}


def cmd_points_bp(args):
    with Session(args) as s:
        addr = s.addr(args.addr)
        return {"id": s.cl.breakpoint_add(addr, args.cpu, s.console), "addr": addr}


def cmd_points_wp(args):
    with Session(args) as s:
        addr = s.addr(args.addr)
        write = args.write or not args.read
        return {"id": s.cl.watchpoint_add(addr, args.size, args.read, write, args.cpu, s.console), "addr": addr}


def cmd_points_remove(args):
    with Session(args) as s:
        s.cl.point_remove(args.id, s.console)
        return {"ok": True}


def cmd_points_enable(args):
    with Session(args) as s:
        s.cl.point_enable(args.id, args.enabled, s.console)
        return {"ok": True}


# ----------------------------------------------------------------------------- trace

def cmd_trace_config(args):
    with Session(args) as s:
        start = s.addr(args.start) if args.start is not None else 0
        end = s.addr(args.end) if args.end is not None else 0xFFFFFFFF
        s.cl.trace_config(args.capacity, args.cpu, start, end, s.console)
        return {"ok": True, "capacity": args.capacity}


def cmd_trace_read(args):
    with Session(args) as s:
        entries, truncated = s.cl.trace_read(args.max, args.with_symbols, s.console)
        return {"entries": entries, "truncated": truncated}


# ----------------------------------------------------------------------------- calls, coverage, writers, packets

def cmd_calls_track(args):
    with Session(args) as s:
        s.cl.calls_track(args.state, s.console)
        return {"ok": True}


def cmd_calls_stack(args):
    with Session(args) as s:
        return {"frames": s.cl.callstack(args.cpu, args.with_symbols, s.console)}


def cmd_calls_counts(args):
    with Session(args) as s:
        counts, truncated = s.cl.call_counts_read(args.cpu, args.max, args.with_symbols, s.console)
        return {"counts": counts, "truncated": truncated}


def cmd_coverage_track(args):
    with Session(args) as s:
        s.cl.coverage_track(args.state, s.console)
        return {"ok": True}


def cmd_coverage_read(args):
    with Session(args) as s:
        hits, truncated = s.cl.coverage_read(args.cpu, args.max, args.with_symbols, s.console)
        if args.with_symbols:
            return {"addresses": [a for a, _ in hits], "names": [n for _, n in hits], "truncated": truncated}
        return {"addresses": hits, "truncated": truncated}


def cmd_writers_track(args):
    with Session(args) as s:
        s.cl.writers_track(args.state, s.console)
        return {"ok": True}


def cmd_writers_get(args):
    with Session(args) as s:
        addr = s.addr(args.addr)
        return {"addr": addr, "writer": s.cl.last_writer(addr, s.console)}


def cmd_packets_track(args):
    with Session(args) as s:
        s.cl.packets_track(args.state, args.capacity, s.console)
        return {"ok": True}


def cmd_packets_read(args):
    with Session(args) as s:
        packets, truncated = s.cl.packets_read(args.max, s.console)
        for p in packets:
            p["data"] = p["data"].hex()
        return {"packets": packets, "truncated": truncated}


# ----------------------------------------------------------------------------- lua

def lua_result(s, args, frames=0):
    if frames:
        s.cl.run(frames, [s.console], timeout=args.timeout)
    time.sleep(args.wait)
    out = s.cl.lua_text(s.console, timeout=0.1)
    return {"ok": True, "output": out, "breaks": [break_dict(e) for e in s.cl.take_events("break")]}


def cmd_lua_exec(args):
    with Session(args) as s:
        s.cl.lua_exec(args.code, s.console)
        return lua_result(s, args, args.frames)


def cmd_lua_run(args):
    with Session(args) as s:
        if args.inline:
            s.cl.lua_run(args.script, False, s.console)
        else:
            s.cl.lua_run(os.path.abspath(args.script), True, s.console)
        return lua_result(s, args, args.frames)


def cmd_lua_stop(args):
    with Session(args) as s:
        s.cl.lua_stop(s.console)
        return {"ok": True}


# ----------------------------------------------------------------------------- registration

def register(sub, leaf):
    def cpu_both(p, default=9):
        p.add_argument("--cpu", type=cpu_arg, default=default, help="9, 7 or both")

    def cpu_one(p):
        p.add_argument("--cpu", type=int, choices=(9, 7), default=9)

    def sym(p):
        p.add_argument("--with-symbols", action="store_true", help="add server-side symbol names")

    pts = sub.add_parser("points", help="breakpoints and watchpoints").add_subparsers(dest="sub", required=True)
    leaf(pts, "list", "list points with hit counts", cmd_points_list)
    p = leaf(pts, "bp", "add an execution breakpoint", cmd_points_bp)
    p.add_argument("addr")
    cpu_both(p)
    p = leaf(pts, "wp", "add a watchpoint (default: write)", cmd_points_wp)
    p.add_argument("addr")
    p.add_argument("size", type=int)
    p.add_argument("--read", action="store_true")
    p.add_argument("--write", action="store_true")
    cpu_both(p)
    p = leaf(pts, "remove", "remove a point", cmd_points_remove)
    p.add_argument("id", type=int)
    for name, flag in (("enable", True), ("disable", False)):
        p = leaf(pts, name, f"{name} a point", cmd_points_enable)
        p.add_argument("id", type=int)
        p.set_defaults(enabled=flag)

    tr = sub.add_parser("trace", help="instruction trace").add_subparsers(dest="sub", required=True)
    p = leaf(tr, "config", "set the trace ring (capacity 0 turns it off)", cmd_trace_config)
    p.add_argument("--capacity", type=int, required=True)
    cpu_both(p, "both")
    p.add_argument("--start")
    p.add_argument("--end")
    p = leaf(tr, "read", "take the trace (clears it)", cmd_trace_read)
    p.add_argument("--max", type=int, default=0)
    sym(p)

    ca = sub.add_parser("calls", help="call tracking").add_subparsers(dest="sub", required=True)
    p = leaf(ca, "track", "turn call tracking on or off", cmd_calls_track)
    p.add_argument("state", type=onoff)
    p = leaf(ca, "stack", "shadow call stack, innermost first", cmd_calls_stack)
    cpu_one(p)
    sym(p)
    p = leaf(ca, "counts", "per-function call counts (clears them)", cmd_calls_counts)
    cpu_one(p)
    p.add_argument("--max", type=int, default=0)
    sym(p)

    co = sub.add_parser("coverage", help="executed-address coverage").add_subparsers(dest="sub", required=True)
    p = leaf(co, "track", "turn coverage on or off", cmd_coverage_track)
    p.add_argument("state", type=onoff)
    p = leaf(co, "read", "executed addresses since the last read (clears)", cmd_coverage_read)
    cpu_one(p)
    p.add_argument("--max", type=int, default=0)
    sym(p)

    lw = sub.add_parser("last-writer", help="who last wrote a main-RAM word").add_subparsers(dest="sub", required=True)
    p = leaf(lw, "track", "turn writer tracking on or off", cmd_writers_track)
    p.add_argument("state", type=onoff)
    p = leaf(lw, "get", "the last writer of ADDR", cmd_writers_get)
    p.add_argument("addr")

    pk = sub.add_parser("packets", help="wireless packet log").add_subparsers(dest="sub", required=True)
    p = leaf(pk, "track", "turn packet logging on or off", cmd_packets_track)
    p.add_argument("state", type=onoff)
    p.add_argument("--capacity", type=int, default=4096)
    p = leaf(pk, "read", "packets since the last read (clears)", cmd_packets_read)
    p.add_argument("--max", type=int, default=0)

    lua = sub.add_parser("lua", help="Lua scripts").add_subparsers(dest="sub", required=True)

    def lua_opts(p):
        p.add_argument("--frames", type=int, default=0, help="run this many frames before collecting output")
        p.add_argument("--wait", type=float, default=0.3, help="seconds to wait for output (default 0.3)")

    p = leaf(lua, "exec", "execute a Lua snippet and print its output", cmd_lua_exec)
    p.add_argument("code")
    lua_opts(p)
    p = leaf(lua, "run", "start a Lua script file (hooks stay active until `lua stop`)", cmd_lua_run)
    p.add_argument("script")
    p.add_argument("--inline", action="store_true", help="SCRIPT is source text, not a file")
    lua_opts(p)
    leaf(lua, "stop", "stop the console's Lua script", cmd_lua_stop)
