"""Per-console symbol tables and per-game watch lists in the MCP server (tool functions called directly).

The `mcp`, `PIL`, `numpy`, `yaml` and `capstone` packages are replaced by stubs when they are not installed."""
import json
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

from .base import have_library


def _install_stubs():
    try:
        import mcp.server.fastmcp  # noqa: F401
    except ImportError:
        class FastMCP:
            def __init__(self, *args, **kwargs):
                pass

            def tool(self, *args, **kwargs):
                return lambda fn: fn

            def run(self):
                pass

        class Image:
            def __init__(self, data=None, format=None):
                self.data, self.format = data, format

        for name in ("mcp", "mcp.server", "mcp.server.fastmcp"):
            sys.modules[name] = types.ModuleType(name)
        sys.modules["mcp"].__path__ = []
        sys.modules["mcp.server"].__path__ = []
        sys.modules["mcp.server.fastmcp"].FastMCP = FastMCP
        sys.modules["mcp.server.fastmcp"].Image = Image
    for name in ("numpy", "yaml", "capstone"):
        try:
            __import__(name)
        except ImportError:
            mod = types.ModuleType(name)
            if name == "capstone":
                class Cs:
                    detail = False

                    def __init__(self, *args):
                        pass

                    def disasm(self, code, addr, count=0):
                        return []  # every word shows as .word; enough for symbol labels

                mod.Cs, mod.CS_ARCH_ARM, mod.CS_MODE_ARM, mod.CS_MODE_THUMB = Cs, 0, 0, 1
            sys.modules[name] = mod
    try:
        from PIL import Image as _  # noqa: F401
    except ImportError:
        pil = types.ModuleType("PIL")
        pil.__path__ = []
        pil.Image = types.ModuleType("PIL.Image")
        sys.modules["PIL"], sys.modules["PIL.Image"] = pil, pil.Image


_install_stubs()
from melonds_mcp import server  # noqa: E402

RAM = 0x02200000


@unittest.skipUnless(have_library(), "headless library not available")
class McpServerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(self._destroy_all)

    def _destroy_all(self):
        for i in list(server.CONSOLES):
            server.console_destroy(i)

    def make(self, **kw):
        i = server.console_create(**kw)["console"]
        server.console_reset(i)
        return i

    def symfile(self, name, text):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def two_tables(self):
        a = self.symfile("a.sym", "02000100 foo 0x20\n02000300 only_a 4\n")
        b = self.symfile("b.sym", "02000200 foo 0x20\n02000400 only_b 4\n")
        c0, c1 = self.make(), self.make()
        server.symbols_load(a, console=c0)
        server.symbols_load(b, console=c1)
        return c0, c1

    def fake_rom(self, console, path):
        with mock.patch.object(server.Console, "load_rom", lambda self, p, s=None: setattr(self, "rom", str(p))):
            server.rom_load(path, console=console)

    # symbols

    def test_same_name_resolves_per_console(self):
        c0, c1 = self.two_tables()
        self.assertEqual(server.symbol_lookup("foo", console=c0)["addr"], "02000100")
        self.assertEqual(server.symbol_lookup("foo", console=c1)["addr"], "02000200")
        self.assertEqual(server.addr_of("foo", server.CONSOLES[c0]), 0x02000100)
        self.assertEqual(server.addr_of("foo", server.CONSOLES[c1]), 0x02000200)

    def test_symbols_load_on_console_1_leaves_console_0(self):
        c0 = self.make()
        c1 = self.make()
        server.symbols_load(self.symfile("b.sym", "02000200 foo 0x20\n"), console=c1)
        with self.assertRaises(server.MelonError):
            server.symbol_lookup("foo", console=c0)
        self.assertEqual(server.describe(0x02000200, server.CONSOLES[c0]), "02000200")
        self.assertEqual(server.describe(0x02000200, server.CONSOLES[c1]), "foo")

    def test_describe_differs(self):
        c0, c1 = self.two_tables()
        self.assertEqual(server.describe(0x02000100, server.CONSOLES[c0]), "foo")
        self.assertEqual(server.describe(0x02000100, server.CONSOLES[c1]), "02000100")
        self.assertEqual(server.describe(0x02000204, server.CONSOLES[c1]), "foo+0x4")
        self.assertEqual(server.symbol_lookup("02000300", console=c0)["symbol"], "only_a")

    def test_memory_tools_use_target_table(self):
        c0, c1 = self.two_tables()
        text0 = server.disassemble_at("0x02000100", 1, console=c0)
        text1 = server.disassemble_at("0x02000100", 1, console=c1)
        self.assertIn("foo:", text0)
        self.assertNotIn("foo", text1)
        server.memory_write("0x02000300", "78563412", console=c0)
        server.memory_write("0x02000300", "78563412", console=c1)
        hits0 = server.memory_search("0x12345678", start="0x02000300", length=16, console=c0)
        hits1 = server.memory_search("0x12345678", start="0x02000300", length=16, console=c1)
        self.assertEqual(hits0, ["02000300 only_a"])
        self.assertEqual(hits1, ["02000300 02000300"])

    def test_symbol_name_only_known_on_its_console(self):
        c0, c1 = self.two_tables()
        self.assertEqual(server.memory_read("only_a", 4, console=c0).split()[0], "02000300")
        with self.assertRaises(server.MelonError):
            server.memory_read("only_a", 4, console=c1)

    def test_overlays_loaded_empty_without_dsd_table(self):
        c0, c1 = self.two_tables()
        self.assertEqual(server.overlays_loaded(console=c0), [])
        self.assertEqual(server.overlays_loaded(console=c1), [])

    def test_console_create_symbols_parameter(self):
        a = self.symfile("a.sym", "02000100 foo 0x20\n")
        c0 = self.make(symbols=a)
        c1 = self.make()
        self.assertEqual(server.symbol_lookup("foo", console=c0)["addr"], "02000100")
        with self.assertRaises(server.MelonError):
            server.symbol_lookup("foo", console=c1)

    def test_destroy_drops_table(self):
        c0, c1 = self.two_tables()
        server.console_destroy(c0)
        self.assertEqual(server.describe(0x02000100, None), "02000100")
        self.assertNotIn(c0, server.SYMBOLS)
        self.assertIn(c1, server.SYMBOLS)
        c2 = self.make()
        with self.assertRaises(server.MelonError):
            server.symbol_lookup("only_a", console=c2)

    # watches

    def watch_values(self, console):
        server.memory_write(hex(RAM), (100 + console).to_bytes(4, "little").hex(), console=console)

    def test_same_rom_shares_watches(self):
        c0, c1 = self.make(), self.make()
        self.fake_rom(c0, "game.nds")
        self.fake_rom(c1, os.path.join(".", "game.nds"))
        self.watch_values(c0)
        self.watch_values(c1)
        server.watch_add("hp", hex(RAM), console=c0)
        self.assertEqual(server.watch_read(console=c1), {"hp": 100 + c1})
        self.assertEqual(server.watch_read(console=c0), {"hp": 100 + c0})
        server.watch_remove("hp", console=c1)
        self.assertEqual(server.watch_read(console=c0), {})

    def test_different_roms_do_not_share(self):
        c0, c1 = self.make(), self.make()
        self.fake_rom(c0, "game_a.nds")
        self.fake_rom(c1, "game_b.nds")
        server.watch_add("hp", hex(RAM), console=c0)
        self.assertEqual(list(server.watch_read(console=c0)), ["hp"])
        self.assertEqual(server.watch_read(console=c1), {})

    def test_no_rom_consoles_have_own_lists(self):
        c0, c1 = self.make(), self.make()
        server.watch_add("hp", hex(RAM), console=c0)
        self.assertEqual(server.watch_read(console=c1), {})
        self.assertEqual(list(server.watch_read(console=c0)), ["hp"])

    def test_destroy_keeps_list_while_game_still_running(self):
        c0, c1 = self.make(), self.make()
        self.fake_rom(c0, "game.nds")
        self.fake_rom(c1, "game.nds")
        server.watch_add("hp", hex(RAM), console=c0)
        server.console_destroy(c0)
        self.assertEqual(list(server.watch_read(console=c1)), ["hp"])
        server.console_destroy(c1)
        self.assertEqual(server.WATCH_LISTS, {})

    def test_watch_save_load_format(self):
        c0, c1 = self.make(), self.make()
        server.watch_add("pos", hex(RAM), "fx32", 9, 0, "x pos", console=c0)
        server.watch_add("name", hex(RAM + 16), "ascii", 9, 8, console=c0)
        path = os.path.join(self.tmp.name, "w.json")
        server.watch_save(path, console=c0)
        with open(path) as f:
            data = json.load(f)
        self.assertEqual(data["pos"], {"addr": RAM, "type": "fx32", "cpu": 9, "length": 0, "note": "x pos"})
        self.assertEqual(data["name"]["length"], 8)
        self.assertEqual(server.watch_load(path, console=c1), ["pos", "name"])
        self.assertEqual(list(server.watch_read(console=c1)), ["pos", "name"])
        self.assertEqual(server.watch_read(console=c0).keys(), data.keys())

    def test_watch_add_resolves_symbol_in_target_console(self):
        c0, c1 = self.two_tables()
        server.watch_add("w", "foo", console=c1)
        self.assertEqual(server.WATCH_LISTS[server.watch_key(server.CONSOLES[c1])].items["w"]["addr"], 0x02000200)


if __name__ == "__main__":
    unittest.main()
