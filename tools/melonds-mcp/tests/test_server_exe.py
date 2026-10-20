"""The melonds_server executable: ready line, port search, port file, consoles, name, shutdown, signals.

Not covered (needs a commercial game): two consoles in one process completing a local wireless session.
"""
import glob
import os
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

from melonds_mcp.client import Client, ServerError
from melonds_mcp.client import T, _str

ROOT = Path(__file__).resolve().parents[3]
READY = re.compile(r"^MELONDS_READY port=(\d+) pid=(\d+) name=(.*)$")


def find_exe():
    env = os.environ.get("MELONDS_SERVER_EXE")
    if env and os.path.exists(env):
        return env
    for pattern in ("build/*/melonds_server.exe", "build/*/melonds_server"):
        hits = sorted(glob.glob(str(ROOT / pattern)), key=os.path.getmtime, reverse=True)
        if hits:
            return hits[0]
    return None


EXE = find_exe()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def free_port_block(n=8):
    """A port P with P..P+n-1 all free right now."""
    for _ in range(50):
        p = free_port()
        if p + n >= 65535:
            continue
        socks = []
        try:
            for i in range(n):
                s = socket.socket()
                s.bind(("127.0.0.1", p + i))
                socks.append(s)
            return p
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
    raise RuntimeError("no free port block")


class Server:
    """A running melonds_server process."""

    def __init__(self, *args):
        env = dict(os.environ)
        extra = os.environ.get("MELONDS_DLL_DIRS", "")
        if extra:
            env["PATH"] = extra + os.pathsep + env.get("PATH", "")
        kwargs = {}
        if sys.platform == "win32":
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        self.proc = subprocess.Popen([EXE, *map(str, args)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, env=env, **kwargs)
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.err = []
        threading.Thread(target=lambda: self.err.extend(self.proc.stderr), daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def line(self, timeout=30):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError("timed out waiting for output; stderr: " + "".join(self.err))

    def ready(self, timeout=30):
        line = self.line(timeout)
        m = READY.match(line or "")
        assert m, f"not a ready line: {line!r}; stderr: {''.join(self.err)}"
        self.port, self.pid, self.name = int(m.group(1)), int(m.group(2)), m.group(3)
        return self

    def wait(self, timeout=15):
        return self.proc.wait(timeout)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)
        for f in (self.proc.stdout, self.proc.stderr):
            f.close()


@unittest.skipUnless(EXE, "melonds_server executable not found (set MELONDS_SERVER_EXE)")
class ServerExeTest(unittest.TestCase):
    def setUp(self):
        self.servers = []
        self.clients = []
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        for c in self.clients:
            c.close()
        for s in self.servers:
            s.stop()
        self.tmp.cleanup()

    def start(self, *args, ready=True):
        s = Server(*args)
        self.servers.append(s)
        return s.ready() if ready else s

    def connect(self, server):
        c = Client(server.port, timeout=20)
        self.clients.append(c)
        return c

    def test_ready_line(self):
        s = self.start("--port", free_port(), "--name", "p2")
        self.assertEqual(s.name, "p2")
        self.assertEqual(s.pid, s.proc.pid)
        info = self.connect(s).hello()
        self.assertEqual(info.port, s.port)
        self.assertEqual(info.pid, s.pid)
        self.assertEqual(info.name, "p2")
        self.assertEqual(info.ids, [0])

    def test_ready_line_is_the_only_stdout_line_before_shutdown(self):
        s = self.start("--port", 0)
        time.sleep(0.3)
        self.assertTrue(s.lines.empty())

    def test_same_port_twice_lands_on_two_ports(self):
        base = free_port_block()
        a = self.start("--port", base)
        b = self.start("--port", base)
        self.assertEqual(a.port, base)
        self.assertNotEqual(a.port, b.port)
        self.assertTrue(base < b.port < base + 32)
        self.assertEqual(self.connect(a).hello().port, a.port)
        self.assertEqual(self.connect(b).hello().port, b.port)

    def test_port_range_one_on_taken_port(self):
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(1)
        self.addCleanup(blocker.close)
        port = blocker.getsockname()[1]
        s = self.start("--port", port, "--port-range", 1, ready=False)
        self.assertEqual(s.line(), f"MELONDS_ERROR no free port in {port}-{port}")
        self.assertEqual(s.wait(), 2)

    def test_port_range_error_names_the_range(self):
        socks = []
        base = free_port_block(3)
        for i in range(3):
            b = socket.socket()
            b.bind(("127.0.0.1", base + i))
            b.listen(1)
            socks.append(b)
        self.addCleanup(lambda: [b.close() for b in socks])
        s = self.start("--port", base, "--port-range", 3, ready=False)
        self.assertEqual(s.line(), f"MELONDS_ERROR no free port in {base}-{base + 2}")
        self.assertEqual(s.wait(), 2)

    def test_port_zero(self):
        s = self.start("--port", 0)
        self.assertGreater(s.port, 0)
        self.assertEqual(self.connect(s).hello().port, s.port)

    def test_port_file(self):
        path = Path(self.tmp.name) / "port.txt"
        s = self.start("--port", 0, "--port-file", path)
        self.assertEqual(path.read_text(), f"{s.port}\n")
        self.assertEqual([p.name for p in Path(self.tmp.name).iterdir()], ["port.txt"])

    def test_consoles(self):
        s = self.start("--port", 0, "--consoles", 3)
        self.assertEqual(self.connect(s).hello().ids, [0, 1, 2])

    def test_default_name_and_console(self):
        s = self.start("--port", 0)
        info = self.connect(s).hello()
        self.assertEqual(info.name, s.name)
        self.assertEqual(info.ids, [0])

    def test_instance_base_visible_to_lua(self):
        s = self.start("--port", 0, "--consoles", 2, "--instance-base", 2)
        cl = self.connect(s)
        cl.hello()
        seen = []
        for console in (0, 1):
            try:
                cl.request(T.lua_exec, _str("print(emu.getconsoleid())"), console)
            except ServerError as e:
                self.skipTest(f"server does not implement lua_exec yet: {e}")
            text = ""
            while True:
                ev = cl.next_event("lua_output", timeout=5, console=console)
                if ev is None:
                    break
                text += ev.text
                if text.strip():
                    break
            seen.append(text.strip())
        self.assertEqual(seen, ["2", "3"])

    def test_bad_arguments(self):
        for args in (["--bogus"], ["--port"], ["--port", "x"], ["--consoles", 0], ["--consoles", 17],
                     ["--instance-base", -1]):
            s = self.start(*args, ready=False)
            line = s.line()
            self.assertTrue(line.startswith("MELONDS_ERROR "), (args, line))
            self.assertEqual(s.wait(), 1, args)

    def test_bad_bios(self):
        bad = Path(self.tmp.name) / "bios.bin"
        bad.write_bytes(b"nope")
        s = self.start("--port", 0, "--bios9", bad, ready=False)
        self.assertTrue(s.line().startswith("MELONDS_ERROR "))
        self.assertEqual(s.wait(), 1)

    def test_bad_rom(self):
        s = self.start("--port", 0, "--rom", Path(self.tmp.name) / "missing.nds", ready=False)
        self.assertTrue(s.line().startswith("MELONDS_ERROR "))
        self.assertEqual(s.wait(), 1)

    def test_help(self):
        s = self.start("--help", ready=False)
        out = []
        while (line := s.line()) is not None:
            out.append(line)
        self.assertEqual(s.wait(), 0)
        self.assertIn("--port-range", "\n".join(out))
        self.assertFalse(any(l.startswith("MELONDS_READY") for l in out))

    def test_env_vars_are_defaults(self):
        bad = Path(self.tmp.name) / "bios.bin"
        bad.write_bytes(b"nope")
        old = os.environ.get("MELONDS_BIOS9")
        os.environ["MELONDS_BIOS9"] = str(bad)
        try:
            s = self.start("--port", 0, ready=False)
            self.assertTrue(s.line().startswith("MELONDS_ERROR "))
            self.assertEqual(s.wait(), 1)
        finally:
            if old is None:
                del os.environ["MELONDS_BIOS9"]
            else:
                os.environ["MELONDS_BIOS9"] = old

    def test_shutdown_message_exits_zero(self):
        s = self.start("--port", 0)
        cl = self.connect(s)
        cl.hello()
        cl.shutdown()
        self.assertEqual(s.wait(10), 0)

    def test_server_accepts_next_client_after_disconnect(self):
        s = self.start("--port", 0)
        self.connect(s).hello()
        self.clients.pop().close()
        self.assertEqual(self.connect(s).hello().ids, [0])
        self.assertIsNone(s.proc.poll())

    def test_interrupt_signal_exits_cleanly(self):
        s = self.start("--port", 0)
        if sys.platform == "win32":
            s.proc.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            s.proc.send_signal(signal.SIGTERM)
        self.assertEqual(s.wait(10), 0)

    def test_kill_leaves_no_process(self):
        s = self.start("--port", 0)
        s.proc.kill()
        s.wait(10)
        self.assertIsNotNone(s.proc.poll())


if __name__ == "__main__":
    unittest.main()
