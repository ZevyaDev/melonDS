"""API level 5 (merge/upstream-debug-2): mds_overlays, and the GDB stub's per-connection reset, EOF handling,
extended mode and "monitor overlays", with the stub still on 127.0.0.1 by default."""
import socket
import time
import unittest

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE9, ConsoleTest
from .test_api_fixes import free_port, lan_address, can_connect


class Level5Features(unittest.TestCase):
    def test_version_and_features(self):
        self.assertGreaterEqual(native.api_version(), 5)
        for name in ("rom_overlays", "gdb_monitor_overlays", "gdb_session_reset", "gdb_extended", "mds_overlays",
                     "gdb_host", "symbols_rom"):
            with self.subTest(name):
                self.assertTrue(native.has(name))


class Overlays(ConsoleTest):
    def test_no_rom_no_overlays(self):
        self.assertEqual(self.c.overlays(), [])
        lib = native.lib()
        self.assertEqual(lib.mds_overlays(self.c.id, None, None, None, 0), 0)

    def test_negative_max_fails(self):
        lib = native.lib()
        self.assertEqual(lib.mds_overlays(self.c.id, None, None, None, -1), -1)
        self.assertIn("max must not be negative", lib.mds_error().decode())

    def test_works_without_the_debugger(self):
        d = native.Console(debug=False)
        d.reset()
        self.addCleanup(d.close)
        self.assertEqual(d.overlays(), [])

    def test_unknown_console(self):
        lib = native.lib()
        self.assertEqual(lib.mds_overlays(9999, None, None, None, 0), -1)
        self.assertIn("no console 9999", lib.mds_error().decode())


def checksum(body):
    return b"%02x" % (sum(body) & 0xFF)


class Rsp:
    """A minimal GDB remote-protocol client."""

    def __init__(self, port, timeout=10):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""
        self.ack = True
        self.s.sendall(b"+")             # the stub waits for an initial ack

    def close(self):
        self.s.close()

    def _fill(self):
        data = self.s.recv(4096)
        if not data:
            raise EOFError("connection closed")
        self.buf += data

    def read_packet(self):
        """The next $...#xx packet's body; returns (body, acks seen before it)."""
        acks = b""
        while True:
            while self.buf[:1] in (b"+", b"-"):
                acks += self.buf[:1]
                self.buf = self.buf[1:]
            start = self.buf.find(b"$")
            end = self.buf.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buf) >= end + 3:
                body = self.buf[start + 1:end]
                self.buf = self.buf[end + 3:]
                if self.ack:
                    self.s.sendall(b"+")
                return body, acks
            self._fill()

    def send(self, body):
        if isinstance(body, str):
            body = body.encode()
        self.s.sendall(b"$" + body + b"#" + checksum(body))

    def command(self, body):
        """Sends a packet; returns (reply bodies up to the final non-O one, acks seen)."""
        self.send(body)
        replies, acks = [], b""
        while True:
            reply, a = self.read_packet()
            acks += a
            replies.append(reply)
            if not (reply.startswith(b"O") and reply != b"OK"):
                return replies, acks

    def interrupt(self):
        self.s.sendall(b"\x03")
        return self.read_packet()[0]


def monitor(rsp, text):
    replies, _ = rsp.command(b"qRcmd," + text.encode().hex().encode())
    out = b"".join(bytes.fromhex(r[1:].decode()) for r in replies[:-1]).decode()
    return out, replies[-1]


class GdbStub(ConsoleTest):
    def setUp(self):
        super().setUp()
        a = A.Asm(CODE9)
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.p9, self.p7 = free_port(), free_port()
        self.c.gdb(self.p9, self.p7)
        self.c.start()
        self.addCleanup(self.stop)

    def stop(self):
        self.c.stop_after(0)
        self.c.wait(10000)
        self.c.gdb(0, 0)

    def connect(self):
        deadline = time.time() + 10
        while True:
            try:
                return Rsp(self.p9)
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)

    def test_listens_on_loopback_only(self):
        self.assertTrue(can_connect("127.0.0.1", self.p9))
        lan = lan_address()
        if lan:
            self.assertFalse(can_connect(lan, self.p9))

    def test_monitor_overlays_without_rom(self):
        rsp = self.connect()
        try:
            stop = rsp.interrupt()
            self.assertTrue(stop.startswith((b"S", b"T")), stop)
            out, final = monitor(rsp, "overlays")
            self.assertEqual(final, b"OK")
            self.assertEqual(out, "no overlays loaded\n")
            _, final = monitor(rsp, "nosuchcommand")
            self.assertTrue(final.startswith(b"E"), final)
            self.assertEqual(rsp.command(b"D")[0][-1], b"OK")
        finally:
            rsp.close()

    def test_state_resets_per_connection(self):
        # first session: no-ack mode, then the client vanishes (EOF)
        rsp = self.connect()
        rsp.interrupt()
        replies, _ = rsp.command(b"QStartNoAckMode")
        self.assertEqual(replies[-1], b"OK")
        rsp.ack = False
        rsp.close()
        # second session: acks are back on, and the stub still answers
        rsp = self.connect()
        try:
            stop = rsp.interrupt()
            self.assertTrue(stop.startswith((b"S", b"T")), stop)
            replies, acks = rsp.command(b"?")
            self.assertIn(b"+", acks)
            self.assertTrue(replies[-1].startswith((b"S", b"T")))
            self.assertEqual(rsp.command(b"D")[0][-1], b"OK")
        finally:
            rsp.close()

    def test_extended_mode_kill_keeps_the_connection(self):
        rsp = self.connect()
        try:
            rsp.interrupt()
            self.assertEqual(rsp.command(b"!")[0][-1], b"OK")
            self.assertEqual(rsp.command(b"vKill;1")[0][-1], b"OK")
            replies, _ = rsp.command(b"?")
            self.assertTrue(replies[-1].startswith((b"S", b"T")), replies)
            self.assertEqual(rsp.command(b"D")[0][-1], b"OK")
        finally:
            rsp.close()

    def test_kill_without_extended_mode_ends_the_session(self):
        rsp = self.connect()
        try:
            rsp.interrupt()
            self.assertEqual(rsp.command(b"vKill;1")[0][-1], b"OK")
            with self.assertRaises((EOFError, OSError)):
                rsp.command(b"?")
        finally:
            rsp.close()
        # a new session is accepted
        rsp = self.connect()
        try:
            stop = rsp.interrupt()
            self.assertTrue(stop.startswith((b"S", b"T")), stop)
            self.assertEqual(rsp.command(b"D")[0][-1], b"OK")
        finally:
            rsp.close()


if __name__ == "__main__":
    unittest.main()
