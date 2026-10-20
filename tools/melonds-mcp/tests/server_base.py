"""Shared setup for the socket server tests: a console, mds_serve on an OS-picked port, a client."""
import time
import unittest

from melonds_mcp import native
from melonds_mcp.client import Client, ServerError

from .base import CODE7, CODE9, DATA


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.addCleanup(self._cleanup)
        self.clients = []
        self.extra = []
        self.console = native.Console()
        self.console.reset()
        self.c = self.console
        self.port = native.serve(0)

    def _cleanup(self):
        native.set_pacing(0)
        for cl in self.clients:
            cl.close()
        native.serve_stop()
        for con in [self.console] + self.extra:
            con.close()

    def new_console(self):
        con = native.Console()
        con.reset()
        self.extra.append(con)
        return con

    def connect(self, hello=True, **hello_args):
        cl = Client(self.port, timeout=20)
        self.clients.append(cl)
        if hello:
            self.info = cl.hello(**hello_args)
        return cl

    def expect_error(self, name, fn, *args, **kwargs):
        with self.assertRaises(ServerError) as cm:
            fn(*args, **kwargs)
        self.assertEqual(cm.exception.name, name, str(cm.exception))
        return cm.exception

    def wait_until(self, pred, timeout=20, what="condition"):
        deadline = time.time() + timeout
        while not pred():
            self.assertLess(time.time(), deadline, f"timed out waiting for {what}")
            time.sleep(0.002)

    def word(self, addr, cpu=9, con=None):
        return int.from_bytes((con or self.c).read(addr, 4, cpu=cpu), "little")
