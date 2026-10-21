"""MCP tools that match the melonds CLI and native.py (tool functions called directly, as in test_mcp_server).

Consoles boot FreeBIOS without a cart (see base.py); programs are written to main RAM where needed."""
import io
import json
import os
import tempfile
import unittest

from .base import CODE9, have_library
from .test_mcp_server import RAM, server


@unittest.skipUnless(have_library(), "headless library not available")
class McpToolsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(self._destroy_all)
        self.c = server.console_create()["console"]
        server.console_reset(self.c)

    def _destroy_all(self):
        for i in list(server.CONSOLES):
            server.console_destroy(i)

    def path(self, name):
        return os.path.join(self.tmp.name, name)

    def word(self, addr):
        return int.from_bytes(server.CONSOLES[self.c].read(addr, 4), "little")

    # memory

    def test_memory_write_value_and_file(self):
        server.memory_write(hex(RAM), value="0x1234", size=2, console=self.c)
        self.assertEqual(server.CONSOLES[self.c].read(RAM, 3), b"\x34\x12\x00")
        server.memory_write(hex(RAM), value="-1", size=1, console=self.c)
        self.assertEqual(server.CONSOLES[self.c].read(RAM, 1), b"\xff")
        blob = self.path("blob.bin")
        with open(blob, "wb") as f:
            f.write(b"\xde\xad\xbe\xef")
        self.assertEqual(server.memory_write(hex(RAM + 8), path=blob, console=self.c), "wrote 4 bytes")
        self.assertEqual(self.word(RAM + 8), 0xEFBEADDE)

    def test_memory_write_needs_exactly_one_source(self):
        with self.assertRaises(server.MelonError):
            server.memory_write(hex(RAM), console=self.c)
        with self.assertRaises(server.MelonError):
            server.memory_write(hex(RAM), "00", value="1", console=self.c)
        server.memory_write(hex(RAM), "0102", console=self.c)  # positional hex_bytes still works
        self.assertEqual(server.CONSOLES[self.c].read(RAM, 2), b"\x01\x02")

    def test_memory_values(self):
        server.memory_write(hex(RAM), "0100feff00100000", console=self.c)
        self.assertEqual(server.memory_values(hex(RAM), "u16", 2, console=self.c)["values"], [1, 0xFFFE])
        self.assertEqual(server.memory_values(hex(RAM), "s16", 2, console=self.c)["values"], [1, -2])
        self.assertEqual(server.memory_values(hex(RAM + 4), "fx32", console=self.c)["values"], [1.0])
        server.memory_write(hex(RAM + 16), "48690000", console=self.c)
        self.assertEqual(server.memory_values(hex(RAM + 16), "ascii", 8, console=self.c)["values"], ["Hi"])
        with self.assertRaises(server.MelonError):
            server.memory_values(hex(RAM), "u64", console=self.c)

    def test_memory_read_to_file(self):
        server.memory_write(hex(RAM), "cafebabe", console=self.c)
        out = self.path("dump.bin")
        self.assertIn("wrote 4 bytes", server.memory_read(hex(RAM), 4, path=out, console=self.c))
        with open(out, "rb") as f:
            self.assertEqual(f.read(), bytes.fromhex("cafebabe"))

    def test_memory_search_on_arm7_bus(self):
        server.memory_write(hex(RAM), "78563412", console=self.c)
        hits = server.memory_search("0x12345678", start=hex(RAM), length=16, cpu=7, console=self.c)
        self.assertEqual(hits, [f"{RAM:08x} {RAM:08x}"])

    def test_text_find_encodings_and_limit(self):
        server.memory_write(hex(RAM), "Hero".encode().hex() + "00" + "Hero".encode("utf-16-le").hex(), console=self.c)
        both = server.text_find("Hero", hex(RAM), 64, console=self.c)
        self.assertEqual([h.split()[1] for h in both], ["ascii", "utf-16-le", "utf-8"])
        self.assertEqual(server.text_find("Hero", hex(RAM), 64, encodings=["utf16"], console=self.c),
                         [f"{RAM + 5:08x} utf-16-le {RAM + 5:08x}"])
        self.assertEqual(len(server.text_find("Hero", hex(RAM), 64, limit=1, console=self.c)), 1)
        with self.assertRaises(server.MelonError):
            server.text_find("Hero", hex(RAM), 64, encodings=["ebcdic"], console=self.c)

    # screen

    def test_screenshot_screens_and_file(self):
        from PIL import Image
        server.run(2, [self.c])
        for screen, height in (("both", 384), ("top", 192), ("bottom", 192)):
            out = self.path(f"{screen}.png")
            img = server.screenshot(self.c, screen, out)
            self.assertEqual(Image.open(io.BytesIO(img.data)).size, (256, height))
            with open(out, "rb") as f:
                self.assertEqual(f.read(), img.data)
        with self.assertRaises(server.MelonError):
            server.screenshot(self.c, "left")

    # debugger

    def test_breakpoint_on_both_cpus_and_enable(self):
        p = server.breakpoint_add(hex(CODE9), "both", console=self.c)["id"]
        (point,) = server.point_list(console=self.c)
        self.assertEqual((point["cpu"], point["enabled"]), ("both", True))
        self.assertEqual(server.point_enable(p, False, console=self.c), "disabled")
        self.assertFalse(server.point_list(console=self.c)[0]["enabled"])
        server.point_enable(p, console=self.c)
        self.assertTrue(server.point_list(console=self.c)[0]["enabled"])
        with self.assertRaises(server.MelonError):
            server.breakpoint_add(hex(CODE9), "both", condition="true", console=self.c)
        with self.assertRaises(server.MelonError):
            server.point_enable(9999, console=self.c)

    def test_coverage_addresses(self):
        server.coverage_track(True, console=self.c)
        server.run(1, [self.c])
        out = server.coverage_read(console=self.c, addresses=5)
        self.assertGreater(out["instructions"], 0)
        self.assertEqual(len(out["addresses"]), min(5, out["instructions"]))
        self.assertEqual(out["addresses"], sorted(out["addresses"]))
        self.assertNotIn("addresses", server.coverage_read(console=self.c))

    # lua

    def test_lua_exec_and_run_with_frames(self):
        self.assertEqual(server.lua_exec("print(1 + 1)", console=self.c), "2\n")
        out = server.lua_exec("event.onframeend(function() print('f') end)", console=self.c, frames=3)
        self.assertEqual(out[self.c]["state"], "idle")
        self.assertEqual(out["output"].split(), ["f"] * 3)
        out = server.lua_run("print('a') emu.frameadvance() print('b')", console=self.c, frames=2)
        self.assertEqual(out["output"].split(), ["a", "b"])

    # watches and symbols

    def test_watch_list_and_read_names(self):
        server.memory_write(hex(RAM), "0700000009000000", console=self.c)
        server.watch_add("a", hex(RAM), "u32", note="first", console=self.c)
        server.watch_add("b", hex(RAM + 4), "u8", console=self.c)
        listed = server.watch_list(console=self.c)
        self.assertEqual(listed["a"], {"addr": f"{RAM:08x}", "type": "u32", "cpu": 9, "length": 0, "note": "first",
                                       "at": f"{RAM:08x}"})
        self.assertEqual(server.watch_read(console=self.c, names=["b"]), {"b": 9})
        self.assertEqual(server.watch_read(console=self.c), {"a": 7, "b": 9})
        with self.assertRaises(server.MelonError):
            server.watch_read(console=self.c, names=["nope"])
        # watch_list returns copies: the stored address stays a number
        self.assertEqual(server.watches_of(server.CONSOLES[self.c]).items["a"]["addr"], RAM)

    def test_symbols_clear(self):
        sym = self.path("a.sym")
        with open(sym, "w") as f:
            f.write(f"{RAM:08x} hp 4\n")
        server.symbols_load(sym, console=self.c)
        self.assertEqual(server.describe(RAM, server.CONSOLES[self.c]), "hp")
        self.assertEqual(server.symbols_clear(console=self.c), "cleared")
        self.assertEqual(server.describe(RAM, server.CONSOLES[self.c]), f"{RAM:08x}")
        self.assertEqual(server.symbols_clear(console=self.c), "no symbols")


if __name__ == "__main__":
    unittest.main()
