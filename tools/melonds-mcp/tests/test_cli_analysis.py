"""CLI: points/break, trace, calls, coverage, last-writer, symbols, watches, lua."""
from pathlib import Path

from . import armasm as A
from .base import CODE9, DATA
from .cli_base import CliTest


def counter_program():
    """A loop that increments a word at DATA forever; returns (Asm, address of the str)."""
    a = A.Asm(CODE9)
    a.ldr_const(1, DATA)
    a.label("loop")
    a.ldr(0, 1, 0)
    a.add(0, 0, 1)
    a.label("store")
    a.str(0, 1, 0)
    a.b("loop")
    return a


class PointsTests(CliTest):
    def test_breakpoint_reports_break_on_run(self):
        a = counter_program()
        self.load_program(a, CODE9)
        store = a.labels["store"]
        bp = self.cli("points", "bp", hex(store))
        self.assertIn("id", bp)
        listing = self.cli("points", "list")["points"]
        self.assertEqual([(p["id"], p["kind"], p["start"]) for p in listing], [(bp["id"], "exec", store)])
        r = self.cli("run", 5)
        self.assertEqual(len(r["breaks"]), 1)
        b = r["breaks"][0]
        self.assertEqual((b["kind"], b["pc"], b["cpu"]), ("exec", store, 9))
        self.assertEqual(r["consoles"][0]["state"], "parked")
        self.assertEqual(self.cli("points", "list")["points"][0]["hits"], 1)
        self.cli("points", "disable", bp["id"])
        self.assertFalse(self.cli("points", "list")["points"][0]["enabled"])
        self.cli("points", "remove", bp["id"])
        self.assertEqual(self.cli("points", "list")["points"], [])

    def test_watchpoint(self):
        self.load_program(counter_program(), CODE9)
        wp = self.cli("points", "wp", hex(DATA), 4, "--write")
        r = self.cli("run", 3)
        self.assertEqual(r["breaks"][0]["kind"], "write")
        self.assertEqual(r["breaks"][0]["addr"], DATA)
        self.assertEqual(self.cli("points", "list")["points"][0]["id"], wp["id"])


class AnalysisTests(CliTest):
    def test_trace_config_and_read(self):
        a = counter_program()
        self.load_program(a, CODE9)
        self.cli("trace", "config", "--capacity", 64, "--cpu", 9, "--start", hex(CODE9), "--end", hex(CODE9 + 0x40))
        self.cli("run", 1)
        t = self.cli("trace", "read", "--max", 20)
        self.assertEqual(len(t["entries"]), 20)
        self.assertTrue(all(CODE9 <= e["pc"] < CODE9 + 0x40 for e in t["entries"]))
        self.assertEqual(t["entries"][0]["cpu"], 9)
        self.cli("trace", "config", "--capacity", 0)

    def test_coverage_and_calls_and_last_writer(self):
        a = counter_program()
        self.load_program(a, CODE9)
        self.cli("coverage", "track", "on")
        self.cli("calls", "track", "on")
        self.cli("last-writer", "track", "on")
        self.cli("run", 1)
        cov = self.cli("coverage", "read", "--max", 4096)["addresses"]
        self.assertIn(a.labels["store"], cov)
        self.cli("calls", "counts")
        self.assertIn("frames", self.cli("calls", "stack"))
        self.cli("points", "bp", hex(a.labels["store"]))
        self.cli("run", 1)
        lw = self.cli("last-writer", "get", hex(DATA))
        self.assertEqual(lw["writer"]["kind"], "cpu")
        self.assertEqual(lw["writer"]["pc"], a.labels["store"])

    def test_packets(self):
        self.cli("packets", "track", "on", "--capacity", 16)
        self.assertEqual(self.cli("packets", "read")["packets"], [])


class SymbolTests(CliTest):
    def symfile(self):
        p = self.tmpfile("syms.txt")
        Path(p).write_text(f"{CODE9:08x} MainLoop 40\n{DATA:08x} PlayerHp 4\n")
        return p

    def test_symbol_name_in_address_argument(self):
        self.cli("write", hex(DATA + 4), "--hex", "cafebabe")
        r = self.cli("read", "PlayerHp+4", 4, "--symbols", self.symfile())
        self.assertEqual((r["addr"], r["hex"]), (DATA + 4, "cafebabe"))
        self.assertEqual(self.cli("read", "PlayerHp", 4, "--symbols", self.symfile())["addr"], DATA)
        err = self.cli_error("read", "Missing", 4, "--symbols", self.symfile())
        self.assertEqual(err["error"], "unknown_symbol")

    def test_symbols_load_remembers_per_console(self):
        loaded = self.cli("symbols", "load", self.symfile())
        self.assertEqual(loaded["symbols"], 2)
        self.assertEqual(self.cli("read", "PlayerHp", 4)["addr"], DATA)       # remembered, no --symbols
        self.assertEqual(self.cli("symbols", "lookup", "MainLoop")["addr"], CODE9)
        self.assertEqual(self.cli("symbols", "lookup", hex(CODE9 + 8))["name"], "MainLoop+0x8")
        # a trace with server-side names
        self.load_program(counter_program(), CODE9)
        self.cli("trace", "config", "--capacity", 16, "--cpu", 9)
        self.cli("run", 1)
        e = self.cli("trace", "read", "--max", 4, "--with-symbols")["entries"]
        self.assertTrue(any(x.get("name", "").startswith("MainLoop") for x in e), e)
        self.cli("symbols", "clear")
        self.assertEqual(self.cli_error("read", "PlayerHp", 4)["error"], "no_symbols")


class WatchTests(CliTest):
    def test_watch_add_read_for_nocart(self):
        self.cli("watch", "add", "hp", hex(DATA), "--type", "u32")
        self.cli("watch", "add", "speed", hex(DATA + 4), "--type", "fx32", "--note", "units/frame")
        self.cli("write", hex(DATA), "77", "--size", 4)
        self.cli("write", hex(DATA + 4), "0x3000", "--size", 4)
        r = self.cli("watch", "read")
        self.assertEqual(r["game"], "nocart")
        self.assertEqual(r["values"], {"hp": 77, "speed": 3.0})
        self.assertEqual(sorted(w["name"] for w in self.cli("watch", "list")["watches"]), ["hp", "speed"])
        self.cli("watch", "remove", "hp")
        self.assertEqual(self.cli("watch", "read")["values"], {"speed": 3.0})
        self.assertTrue((self.home / "watches" / "nocart.json").exists())

    def test_watch_lists_are_per_game(self):
        self.cli("write", "0x027FFE0C", "--hex", "41424344")
        self.cli("watch", "add", "x", hex(DATA), "--type", "u8")
        self.assertEqual(self.cli("watch", "list")["game"], "ABCD")
        self.assertTrue((self.home / "watches" / "ABCD.json").exists())
        self.assertEqual(self.cli("watch", "list", "--game", "nocart")["watches"], [])

    def test_watch_string_types(self):
        self.write_hex(DATA, b"Link\0")
        self.cli("watch", "add", "name", hex(DATA), "--type", "ascii", "--length", 8)
        self.assertEqual(self.cli("watch", "read")["values"], {"name": "Link"})


class LuaTests(CliTest):
    def test_lua_exec_prints_output(self):
        r = self.cli("lua", "exec", "print('hello from lua')")
        self.assertIn("hello from lua", r["output"])

    def test_lua_run_file_and_stop(self):
        script = self.tmpfile("s.lua")
        Path(script).write_text("print('script loaded')\n")
        r = self.cli("lua", "run", script)
        self.assertIn("script loaded", r["output"])
        self.assertEqual(self.cli("lua", "stop")["ok"], True)
