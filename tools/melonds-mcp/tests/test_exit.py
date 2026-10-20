"""A process that creates consoles and exits without closing them must still exit promptly."""
import subprocess
import sys
import time
import unittest
from pathlib import Path

from melonds_mcp import native

TOOLS = Path(__file__).resolve().parents[1]

# Each script leaves the console (and the server thread) alive on purpose and just returns.
SCRIPTS = {
    "console only": """
c = native.Console()
c.reset()
""",
    "ran a frame": """
c = native.Console()
c.reset()
c.run(1)
""",
    "serving": """
c = native.Console()
c.reset()
c.run(1)
native.serve(0)
""",
    "serving with a client": """
from melonds_mcp.client import Client
c = native.Console()
c.reset()
port = native.serve(0)
cl = Client(port)
cl.hello()
cl.run(2)
""",
    "live and paced": """
c = native.Console()
c.reset()
native.set_pacing(60)
c.start()
native.serve(0)
""",
    "two consoles one parked": """
a = native.Console()
b = native.Console()
a.reset()
b.reset()
a.add_breakpoint(0x02100000)
a.write(0x02100000, bytes(8))
a.set_reg("pc", 0x02100000)
a.run(5)
b.start()
native.serve(0)
""",
}


class ExitTests(unittest.TestCase):
    def run_script(self, body):
        code = "from melonds_mcp import native\n" + body + "\nprint('body done', flush=True)\n"
        proc = subprocess.Popen([sys.executable, "-c", code], cwd=str(TOOLS), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        t0 = time.time()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            self.fail(f"process did not exit within 10 s:\n{out}")
        self.assertIn("body done", out)
        self.assertEqual(proc.returncode, 0, out)
        return time.time() - t0

    def test_exit_without_cleanup(self):
        for name, body in SCRIPTS.items():
            with self.subTest(name):
                self.run_script(body)


if __name__ == "__main__":
    unittest.main()
