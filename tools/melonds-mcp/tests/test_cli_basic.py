"""CLI: launch/attach/shutdown, consoles, run, errors, memory, registers."""
from pathlib import Path

from . import armasm as A
from .base import CODE9, DATA
from .cli_base import CliTest


class SessionTests(CliTest):
    launch_server = False

    def test_launch_attach_shutdown(self):
        portfile = self.tmpfile("port.txt")
        info = self.cli("launch", "--port", 0, "--name", "alpha", "--port-file", portfile, port=False)
        self.pids.append(info["pid"])
        self.port = info["port"]
        self.assertEqual(info["name"], "alpha")
        self.assertGreater(info["port"], 0)
        self.assertEqual(Path(portfile).read_text().strip(), str(info["port"]))
        # attach by --port-file only, then by --port
        a = self.cli("attach", "--port-file", portfile, port=False)
        self.assertEqual((a["name"], a["port"], a["pid"]), ("alpha", info["port"], info["pid"]))
        self.assertEqual(a["consoles"], [{"id": 0, "state": "idle", "frames": 0}])
        b = self.cli("attach", "--port", info["port"], port=False)
        self.assertEqual(b["name"], "alpha")
        self.assertEqual(self.cli("shutdown")["ok"], True)
        err = self.cli_error("attach")
        self.assertEqual(err["error"], "connect_failed")

    def test_no_server_is_an_error(self):
        err = self.cli_error("attach", port=False)
        self.assertEqual(err["error"], "no_server")

    def test_launch_failure_reports_error(self):
        err = self.cli_error("launch", "--exe", self.tmpfile("missing.exe"), port=False)
        self.assertIn("error", err)

    def test_launch_forwards_server_arguments(self):
        info = self.cli("launch", "--port", 0, "--consoles", 2, "--name", "two", port=False)
        self.pids.append(info["pid"])
        self.port = info["port"]
        self.assertEqual([c["id"] for c in self.cli("attach")["consoles"]], [0, 1])


class ConsoleTests(CliTest):
    def test_consoles_list_create_destroy(self):
        self.assertEqual([c["id"] for c in self.cli("consoles", "list")["consoles"]], [0])
        made = self.cli("consoles", "create")
        self.assertEqual(made["id"], 1)
        self.assertEqual([c["id"] for c in self.cli("consoles", "list")["consoles"]], [0, 1])
        self.cli("consoles", "destroy", 1)
        self.assertEqual([c["id"] for c in self.cli("consoles", "list")["consoles"]], [0])

    def test_run_advances_frames(self):
        r = self.cli("run", 10)
        self.assertEqual(r["frames"], 10)
        self.assertEqual(r["consoles"], [{"id": 0, "state": "idle", "frames": 10}])
        self.assertEqual(r["breaks"], [])
        self.assertEqual(self.cli("attach")["consoles"][0]["frames"], 10)

    def test_run_selected_consoles(self):
        self.cli("consoles", "create")
        r = self.cli("run", 3, "--consoles", "1")
        by_id = {c["id"]: c["frames"] for c in r["consoles"]}
        self.assertEqual(by_id, {1: 3})
        self.assertEqual(self.cli("attach")["consoles"][0]["frames"], 0)

    def test_pause_step_and_live(self):
        self.cli("run", 1)
        r = self.cli("live", "start", "--frames", 5)
        self.assertGreaterEqual(r["consoles"][0]["frames"], 6)
        self.assertEqual(r["consoles"][0]["state"], "idle")
        self.assertEqual(self.cli("live", "stop")["ok"], True)
        self.assertEqual(self.cli("pause")["ok"], True)
        s = self.cli("step", "--count", 3)
        self.assertIn("pc", s)


class MemoryTests(CliTest):
    def test_write_read_hex_and_values(self):
        self.cli("write", hex(DATA), "--hex", "deadbeef")
        r = self.cli("read", hex(DATA), 4)
        self.assertEqual((r["hex"], r["addr"], r["length"]), ("deadbeef", DATA, 4))
        self.cli("write", hex(DATA + 4), "0x12345678", "--size", 4)
        v = self.cli("read", hex(DATA + 4), 4, "--as", "u32")
        self.assertEqual(v["values"], [0x12345678])
        self.cli("write", hex(DATA + 8), "0x2000", "--size", 4)
        self.assertEqual(self.cli("read", hex(DATA + 8), 4, "--as", "fx32")["values"], [2.0])

    def test_read_out_file_and_write_file(self):
        src = self.tmpfile("in.bin")
        Path(src).write_bytes(bytes(range(16)))
        self.cli("write", hex(DATA), "--file", src)
        out = self.tmpfile("out.bin")
        r = self.cli("read", hex(DATA), 16, "--out", out)
        self.assertEqual(Path(out).read_bytes(), bytes(range(16)))
        self.assertEqual(r["out"], out)
        self.assertNotIn("hex", r)

    def test_arm7_read(self):
        self.cli("write", hex(DATA), "--hex", "01020304", "--cpu", 7)
        self.assertEqual(self.cli("read", hex(DATA), 4, "--cpu", 7)["hex"], "01020304")

    def test_search_value_and_bytes(self):
        self.cli("write", hex(DATA + 0x100), "0xC0FFEE11", "--size", 4)
        self.cli("write", hex(DATA + 0x201), "--hex", "a1b2c3d4e5")
        hits = self.cli("search", "0xC0FFEE11", "--size", 4, "--start", hex(DATA), "--length", 0x1000)
        self.assertEqual(hits["addresses"], [DATA + 0x100])
        self.assertFalse(hits["truncated"])
        hits = self.cli("search", "--bytes", "b2c3d4", "--start", hex(DATA), "--length", 0x1000)
        self.assertEqual(hits["addresses"], [DATA + 0x202])

    def test_find_text(self):
        self.write_hex(DATA + 0x40, b"HelloCLI\0")
        self.write_hex(DATA + 0x80, "HelloCLI".encode("utf-16-le"))
        hits = self.cli("find-text", "HelloCLI", "--start", hex(DATA), "--length", 0x1000)["hits"]
        self.assertIn({"addr": DATA + 0x40, "encoding": "ascii"}, hits)
        self.assertIn({"addr": DATA + 0x80, "encoding": "utf16le"}, hits)
        only = self.cli("find-text", "HelloCLI", "--enc", "utf16", "--start", hex(DATA), "--length", 0x1000)["hits"]
        self.assertEqual([h["addr"] for h in only], [DATA + 0x80])

    def test_regs_and_set_reg_and_program(self):
        self.cli("set-reg", "r5", "0x1234")
        regs = self.cli("regs")
        self.assertEqual(regs["regs"]["r5"], 0x1234)
        self.assertEqual(regs["cpu"], 9)
        a = A.Asm(CODE9)
        a.ldr_const(1, DATA)
        a.mov(0, 0x55)
        a.str(0, 1, 0)
        a.label("spin")
        a.b("spin")
        self.load_program(a, CODE9)
        self.cli("run", 2)
        self.assertEqual(self.read_word(DATA), 0x55)


class ErrorTests(CliTest):
    def test_bad_address_is_json_error(self):
        err = self.cli_error("read", "12abc", 4)
        self.assertEqual(err["error"], "bad_address")
        err = self.cli_error("read", "0x02000000", 0x7FFFFFFF)       # more than a message can carry
        self.assertTrue(err["error"])
        self.assertTrue(err["message"])

    def test_bad_arguments_are_json_errors(self):
        self.assertEqual(self.cli_error("read", "nonsense_without_symbols", 4)["error"], "no_symbols")
        self.assertEqual(self.cli_error("input", "Bogus*3")["error"], "bad_argument")
        self.assertEqual(self.cli_error("no-such-command")["error"], "usage")

    def test_unknown_console_error(self):
        self.assertEqual(self.cli_error("run", 1, "--console", 9)["error"], "no_console")
