"""Commands about the server process, consoles, execution, input, savestates, subscriptions, screenshots."""
import glob
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from ..client import encode_input_payload
from . import png
from .common import CliError, Session, break_dict, home_dir
from .sequence import expand

READY = re.compile(r"MELONDS_READY port=(\d+) pid=(\d+) name=(.*)")
ROOT = Path(__file__).resolve().parents[3]
INPUT_CHUNK = 500          # payloads per batch; the server queue holds 600


def find_exe(explicit=None):
    if explicit:
        return explicit
    env = os.environ.get("MELONDS_SERVER_EXE")
    if env:
        return env
    for pattern in ("build/*/melonds_server.exe", "build/*/melonds_server"):
        hits = sorted(glob.glob(str(ROOT / pattern)), key=os.path.getmtime, reverse=True)
        if hits:
            return hits[0]
    raise CliError("no_executable", "melonds_server not found: use --exe or $MELONDS_SERVER_EXE")


# ----------------------------------------------------------------------------- launch / attach

def cmd_launch(args):
    exe = find_exe(args.exe)
    cmd = [exe, *args.server_args]
    if args.port_file:
        cmd += ["--port-file", os.path.abspath(args.port_file)]
    log = Path(args.log) if args.log else home_dir() / "logs" / f"server-{os.getpid()}-{int(time.time())}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    dirs = os.environ.get("MELONDS_DLL_DIRS", "")
    if dirs:
        env["PATH"] = dirs + os.pathsep + env.get("PATH", "")
    kwargs = {}
    if sys.platform == "win32":
        kwargs["creationflags"] = (subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
                                   | subprocess.CREATE_NO_WINDOW)
    else:
        kwargs["start_new_session"] = True
    with open(log, "w") as logf:
        try:
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT, env=env,
                                    close_fds=True, **kwargs)
        except OSError as e:
            raise CliError("launch_failed", f"cannot start {exe}: {e}")
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        text = log.read_text(errors="replace")
        m = READY.search(text)
        if m:
            return {"port": int(m.group(1)), "pid": int(m.group(2)), "name": m.group(3).strip(), "log": str(log)}
        if proc.poll() is not None:
            raise CliError("launch_failed", f"server exited with code {proc.returncode}: {text.strip()}")
        time.sleep(0.05)
    proc.kill()
    raise CliError("launch_failed", "timed out waiting for MELONDS_READY")


def cmd_attach(args):
    with Session(args) as s:
        return {"name": s.info.name, "port": s.info.port, "pid": s.info.pid, "version": s.info.version,
                "default_console": s.info.default_console, "consoles": s.console_states()}


def cmd_shutdown(args):
    with Session(args) as s:
        s.cl.shutdown()
        return {"ok": True}


def cmd_consoles_list(args):
    with Session(args) as s:
        return {"consoles": s.console_states()}


def cmd_consoles_create(args):
    with Session(args) as s:
        cid = s.cl.console_create(args.bios9 or "", args.bios7 or "", args.firmware or "", args.nickname or "")
        return {"id": cid, "consoles": s.console_states()}


def cmd_consoles_destroy(args):
    with Session(args) as s:
        s.cl.console_destroy(args.id)
        return {"ok": True, "consoles": s.console_states()}


def cmd_reset(args):
    with Session(args) as s:
        s.cl.reset(s.console)
        return {"ok": True}


def cmd_rom_load(args):
    with Session(args) as s:
        s.cl.rom_load(os.path.abspath(args.rom), os.path.abspath(args.save) if args.save else "", s.console)
        return {"ok": True}


# ----------------------------------------------------------------------------- execution

def target_ids(s, args):
    text = getattr(args, "consoles", None)
    if text:
        return [int(x, 0) for x in text.split(",") if x.strip()]
    return [s.console]


def cmd_run(args):
    with Session(args) as s:
        ids = target_ids(s, args)
        s.cl.run(args.frames, ids, timeout=args.timeout)
        s.cl.pump(0.05)
        return {"frames": args.frames, "consoles": s.console_states(ids),
                "breaks": [break_dict(e) for e in s.cl.take_events("break")]}


def wait_stopped(s, ids, seconds=5.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if all(c["state"] != "running" for c in s.console_states(ids)):
            return
        time.sleep(0.01)


def cmd_live_start(args):
    with Session(args) as s:
        ids = target_ids(s, args)
        before = {c["id"]: c["frames"] for c in s.console_states(ids)}
        s.cl.live_start(args.fps, ids)
        deadline = time.time() + (args.seconds if args.seconds is not None else args.timeout)
        if args.frames:
            for cid in ids:
                s.cl.subscribe(0x4C495645, [], 1, cid)
            seen = {cid: 0 for cid in ids}
            while min(seen.values()) < args.frames and time.time() < deadline:
                ev = s.cl.next_event("frame", timeout=0.2)
                if ev is not None and ev.console in seen:
                    seen[ev.console] += 1
        elif args.seconds is not None:
            while time.time() < deadline:
                s.cl.pump(min(0.1, max(0.0, deadline - time.time())))
        else:
            raise CliError("usage", "live start needs --frames N or --seconds S (live mode ends when the "
                           "command disconnects)")
        s.cl.live_stop(ids)
        wait_stopped(s, ids)
        for cid in ids:
            try:
                s.cl.unsubscribe(0x4C495645, cid)
            except Exception:
                pass
        s.cl.pump(0.05)
        states = s.console_states(ids)
        for c in states:
            c["ran"] = c["frames"] - before.get(c["id"], 0)
        return {"consoles": states, "breaks": [break_dict(e) for e in s.cl.take_events("break")]}


def cmd_live_stop(args):
    with Session(args) as s:
        ids = target_ids(s, args)
        s.cl.live_stop(ids)
        wait_stopped(s, ids)
        return {"ok": True, "consoles": s.console_states(ids)}


def cmd_pause(args):
    with Session(args) as s:
        s.cl.pause(s.console)
        return {"ok": True, "consoles": s.console_states([s.console])}


def cmd_step(args):
    with Session(args) as s:
        s.cl.step(args.count, args.cpu, s.console)
        regs = s.cl.registers(args.cpu, s.console)
        out = {"cpu": args.cpu, "pc": regs["pc"], "consoles": s.console_states([s.console])}
        name = s.describe(regs["pc"])
        if name:
            out["name"] = name
        return out


# ----------------------------------------------------------------------------- input

def cmd_input(args):
    frames = expand(args.sequence)
    total = max(args.frames or 0, len(frames))
    with Session(args) as s:
        cl = s.cl
        cid = s.console
        ids = [cid]
        cl.set_empty_queue(args.mode, cid)
        start = s.console_states(ids)[0]["frames"]
        sent = 0
        depth = 0
        breaks = []
        for i in range(0, max(len(frames), 1), INPUT_CHUNK):
            part = frames[i:i + INPUT_CHUNK]
            seqs = []
            for f in part:
                seq = cl._next_seq()
                payload = encode_input_payload(seq, f.buttons, False, f.touch)
                seqs.append(cl.send_input_raw(payload, cid))
            for seq in seqs:
                reply = cl._wait_reply(seq)
                depth = int.from_bytes(reply.body[:2], "little")
            sent += len(part)
            if not args.run:
                continue
            last = i + INPUT_CHUNK >= len(frames)
            n = len(part) + (total - len(frames) if last else 0)
            if n:
                cl.run(n, ids, timeout=args.timeout)
            cl.pump(0.02)
            breaks += [break_dict(e) for e in cl.take_events("break")]
            if breaks:
                break
        state = s.console_states(ids)[0]
        return {"console": cid, "payloads": sent, "frames_run": state["frames"] - start if args.run else 0,
                "frame": state["frames"], "state": state["state"], "mode": args.mode, "queue_depth": depth,
                "breaks": breaks}


def cmd_input_log(args):
    with Session(args) as s:
        entries, truncated = s.cl.input_log_read(args.from_frame, args.max, s.console)
        doc = {"console": s.console, "truncated": truncated, "entries": entries}
        if args.out:
            Path(args.out).write_text(json.dumps(doc, indent=1))
            return {"out": args.out, "count": len(entries), "truncated": truncated}
        return doc


# ----------------------------------------------------------------------------- savestates

def cmd_state_save(args):
    with Session(args) as s:
        s.cl.savestate_save(os.path.abspath(args.path), s.console)
        return {"ok": True, "path": os.path.abspath(args.path)}


def cmd_state_load(args):
    with Session(args) as s:
        s.cl.savestate_load(os.path.abspath(args.path), s.console)
        return {"ok": True, "path": os.path.abspath(args.path)}


# ----------------------------------------------------------------------------- subscribe

def parse_range(s, text):
    parts = text.split(":")
    cpu = 9
    if len(parts) == 3:
        cpu = int(parts[2], 0)
    elif len(parts) != 2:
        raise CliError("usage", f"range must be ADDR:LEN[:CPU], not {text!r}")
    return (cpu, s.addr(parts[0]), int(parts[1], 0))


def cmd_subscribe(args):
    with Session(args) as s:
        ranges = [parse_range(s, r) for r in args.range or []]
        sid = 1
        cid = s.console
        s.cl.subscribe(sid, ranges, args.every, cid)
        try:
            s.cl.run(args.frames * args.every, [cid], timeout=args.timeout)
            s.cl.pump(0.05)
            for ev in s.cl.take_events("frame"):
                if ev.console != cid:
                    continue
                print(json.dumps({"console": ev.console, "frame": ev.frame, "applied_seq": ev.applied_seq,
                                  "queue_depth": ev.queue_depth, "dropped": ev.dropped,
                                  "data": [b.hex() for b in ev.blocks.get(sid, [])]}))
        finally:
            s.cl.unsubscribe(sid, cid)
    return None


# ----------------------------------------------------------------------------- screenshot

def cmd_screenshot(args):
    with Session(args) as s:
        frame, pixels = s.cl.screenshot(s.console)
    height = 384
    if args.screen == "top":
        pixels, height = pixels[:256 * 192 * 4], 192
    elif args.screen == "bottom":
        pixels, height = pixels[256 * 192 * 4:], 192
    data = png.encode_bgra(pixels, 256, height, alpha=args.alpha)
    Path(args.out).write_bytes(data)
    return {"file": args.out, "width": 256, "height": height, "frame": frame, "console": s.console}


# ----------------------------------------------------------------------------- registration

def register(sub, leaf):
    p = sub.add_parser("launch", help="start melonds_server detached; unknown options go to the server",
                       allow_abbrev=False)
    p.add_argument("--exe", help="melonds_server path (default $MELONDS_SERVER_EXE, else newest build/*/)")
    p.add_argument("--port-file", help="also have the server write its bound port here")
    p.add_argument("--log", help="server output log (default under the CLI home dir)")
    p.add_argument("--timeout", type=float, default=30.0, help="seconds to wait for the ready line")
    p.set_defaults(func=cmd_launch, server_args=None, passthrough=True)

    leaf(sub, "attach", "connect and print name, port and consoles", cmd_attach)
    leaf(sub, "shutdown", "stop the server process", cmd_shutdown)

    c = sub.add_parser("consoles", help="list, create or destroy consoles").add_subparsers(dest="sub", required=True)
    leaf(c, "list", "list consoles with state and frame count", cmd_consoles_list)
    p = leaf(c, "create", "create a console", cmd_consoles_create)
    for opt in ("bios9", "bios7", "firmware", "nickname"):
        p.add_argument(f"--{opt}")
    p = leaf(c, "destroy", "destroy a console", cmd_consoles_destroy)
    p.add_argument("id", type=int)

    leaf(sub, "reset", "reset the console", cmd_reset)
    p = leaf(sub, "rom-load", "load a ROM into the console", cmd_rom_load)
    p.add_argument("rom")
    p.add_argument("--save")

    p = leaf(sub, "run", "run FRAMES frames and wait; reports breaks", cmd_run)
    p.add_argument("frames", type=int)
    p.add_argument("--consoles", help="comma-separated ids to run together (default: --console)")

    live = sub.add_parser("live", help="free-running mode").add_subparsers(dest="sub", required=True)
    p = leaf(live, "start", "run live for --frames/--seconds, then stop (live ends at disconnect)", cmd_live_start)
    p.add_argument("--frames", type=int)
    p.add_argument("--seconds", type=float)
    p.add_argument("--fps", type=int, default=0, help="0 = unlimited")
    p.add_argument("--consoles")
    p = leaf(live, "stop", "stop live mode", cmd_live_stop)
    p.add_argument("--consoles")

    leaf(sub, "pause", "pause the console", cmd_pause)
    p = leaf(sub, "step", "single-step instructions and print PC", cmd_step)
    p.add_argument("--count", type=int, default=1)
    p.add_argument("--cpu", type=int, choices=(9, 7), default=9)

    p = leaf(sub, "input", 'send a sequence like "A*4 w20 Down*30 touch:128,96*3" and run it', cmd_input)
    p.add_argument("sequence")
    p.add_argument("--mode", choices=("wait", "repeat", "neutral"), default="wait",
                   help="empty-queue mode for frames beyond the sequence (default wait)")
    p.add_argument("--frames", type=int, help="run this many frames in total (at least the sequence length)")
    p.set_defaults(run=True)
    p.add_argument("--run", dest="run", action="store_true", help="run the frames (default)")
    p.add_argument("--no-run", dest="run", action="store_false",
                   help="only queue the payloads (the queue is cleared when the command disconnects)")

    p = leaf(sub, "input-log", "print the applied-input log (JSON); --out saves it", cmd_input_log)
    p.add_argument("--from", dest="from_frame", type=int, default=0)
    p.add_argument("--max", type=int, default=0)
    p.add_argument("--out")

    st = sub.add_parser("state", help="savestates").add_subparsers(dest="sub", required=True)
    p = leaf(st, "save", "save a savestate", cmd_state_save)
    p.add_argument("path")
    p = leaf(st, "load", "load a savestate (clears the input queue)", cmd_state_load)
    p.add_argument("path")

    p = leaf(sub, "subscribe", "collect N frame events (JSON lines) with memory ranges, then unsubscribe",
             cmd_subscribe)
    p.add_argument("--range", action="append", metavar="ADDR:LEN[:CPU]")
    p.add_argument("--every", type=int, default=1, help="an event every N frames")
    p.add_argument("--frames", type=int, default=1, help="number of events to collect")

    p = leaf(sub, "screenshot", "write the screens to a PNG file", cmd_screenshot)
    p.add_argument("out")
    p.add_argument("--screen", choices=("both", "top", "bottom"), default="both")
    p.add_argument("--alpha", action="store_true", help="write RGBA instead of RGB")


__all__ = ["register"]
