"""Shared setup for the CLI tests: a melonds_server started through `launch`, and a `cli()` helper that
runs `python -m melonds_mcp.cli` as a subprocess and parses its JSON output."""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from .test_server_exe import find_exe

MCP_DIR = Path(__file__).resolve().parents[1]
EXE = find_exe()


def wait_exit(pid, seconds=10):
    """Wait for a process to be gone (a dying server still holds its DLLs on Windows)."""
    if sys.platform == "win32":
        import ctypes
        handle = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)      # SYNCHRONIZE
        if handle:
            ctypes.windll.kernel32.WaitForSingleObject(handle, int(seconds * 1000))
            ctypes.windll.kernel32.CloseHandle(handle)
        return
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return
        time.sleep(0.05)


class CliResult:
    def __init__(self, proc):
        self.code = proc.returncode
        self.stdout = proc.stdout
        self.stderr = proc.stderr
        text = proc.stdout.strip()
        self.lines = [json.loads(line) for line in text.splitlines() if line.strip().startswith(("{", "["))]
        self.data = self.lines[0] if len(self.lines) == 1 else (self.lines or None)


@unittest.skipUnless(EXE, "melonds_server executable not found (set MELONDS_SERVER_EXE)")
class CliTest(unittest.TestCase):
    launch_server = True

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name) / "home"
        self.pids = []
        self.port = None
        self.addCleanup(self._kill)
        if self.launch_server:
            self.start_server()

    def tmpfile(self, name):
        return str(Path(self.tmp.name) / name)

    def env(self, port=True):
        env = dict(os.environ)
        env["PYTHONPATH"] = str(MCP_DIR) + os.pathsep + env.get("PYTHONPATH", "")
        env["MELONDS_CLI_HOME"] = str(self.home)
        env["MELONDS_SERVER_EXE"] = EXE
        env.pop("MELONDS_PORT", None)
        if port and self.port:
            env["MELONDS_PORT"] = str(self.port)
        return env

    def run_cli(self, *args, port=True, timeout=120):
        proc = subprocess.run([sys.executable, "-m", "melonds_mcp.cli", *map(str, args)], capture_output=True,
                              text=True, env=self.env(port), cwd=self.tmp.name, timeout=timeout)
        return CliResult(proc)

    def cli(self, *args, **kw):
        """Run a command that must succeed; returns its parsed JSON."""
        r = self.run_cli(*args, **kw)
        self.assertEqual(r.code, 0, f"{args}: {r.stdout} {r.stderr}")
        return r.data

    def cli_error(self, *args, **kw):
        r = self.run_cli(*args, **kw)
        self.assertEqual(r.code, 1, f"{args}: {r.stdout} {r.stderr}")
        self.assertIsInstance(r.data, dict)
        self.assertIn("error", r.data)
        self.assertIn("message", r.data)
        return r.data

    def start_server(self, *extra):
        info = self.cli("launch", "--port", 0, "--name", "clitest", *extra, port=False)
        self.pids.append(info["pid"])
        self.port = info["port"]
        return info

    def _kill(self):
        if self.port:
            try:
                self.run_cli("shutdown", timeout=20)
            except Exception:
                pass
        for pid in self.pids:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
            wait_exit(pid)

    # --------------------------------------------------------------- helpers

    def write_hex(self, addr, data, cpu=9):
        self.cli("write", hex(addr), "--hex", bytes(data).hex(), "--cpu", cpu)

    def read_bytes(self, addr, length, cpu=9):
        return bytes.fromhex(self.cli("read", hex(addr), length, "--cpu", cpu)["hex"])

    def read_word(self, addr, cpu=9):
        return int.from_bytes(self.read_bytes(addr, 4, cpu), "little")

    def load_program(self, code, addr, cpu=9):
        if hasattr(code, "assemble"):
            code = code.assemble()
        self.write_hex(addr, code, cpu)
        self.cli("set-reg", "pc", hex(addr), "--cpu", cpu)


class CliConsole:
    """The slice of the native Console interface that test_input.Recorder.install uses, through the CLI."""

    def __init__(self, test):
        self.t = test

    def write(self, addr, data, cpu=9):
        self.t.write_hex(addr, data, cpu)

    def set_reg(self, reg, value, cpu=9):
        self.t.cli("set-reg", reg, hex(value), "--cpu", cpu)
