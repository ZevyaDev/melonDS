"""Socket server: points, savestates, Lua, trace/call/coverage/writer analysis, packets, pacing, symbols."""
import os
import struct
import tempfile
import time

from melonds_mcp import native
from melonds_mcp.client import T

from . import armasm as A
from .base import CODE7, CODE9, DATA
from .server_base import ServerTest
from .test_server import COUNTER, counter_program


def words(code, base):
    """{address: instruction word} of an assembled ARM program."""
    n = len(code) // 4
    return {base + 4 * i: w for i, w in enumerate(struct.unpack(f"<{n}I", code))}


def call_program(base=CODE9):
    """main calls f1 twice; f1 calls f2. Returns (code, labels)."""
    a = A.Asm(base)
    a.label("main")
    a.bl("f1")
    a.label("second")
    a.bl("f1")
    a.label("done")
    a.b("done")
    a.label("f1")
    a.mov_reg(4, 14)
    a.label("f1_call")
    a.bl("f2")
    a.bx(4)
    a.label("f2")
    a.nop()
    a.mov(2, 7)
    a.bx(14)
    return a.assemble(), dict(a.labels)


def loop_program(base=CODE9):
    """mov r0,#0 ; mov r1,#3 ; loop: add r0,r0,#1 ; subs r1,r1,#1 ; bne loop ; done: b done"""
    a = A.Asm(base)
    a.mov(0, 0)
    a.mov(1, 3)
    a.label("loop")
    a.add(0, 0, 1)
    a.subs(1, 1, 1)
    a.b("loop", A.NE)
    a.label("done")
    a.b("done")
    return a.assemble(), dict(a.labels)


class DebugBase(ServerTest):
    def setUp(self):
        super().setUp()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def path(self, name):
        return os.path.join(self.tmp.name, name)

    def start_at(self, code, base=CODE9, con=None, cpu=9):
        con = con or self.c
        con.write(base, code, cpu=cpu)
        con.set_reg("pc", base, cpu=cpu)

    def run_to_break(self, cl, frames=300, console=0xFFFF):
        cl.run(frames, [] if console == 0xFFFF else [console])
        ev = cl.next_event("break", timeout=5, console=None if console == 0xFFFF else console)
        self.assertIsNotNone(ev, "expected a break")
        return ev


class PointTests(DebugBase):
    def setUp(self):
        super().setUp()
        self.start_at(counter_program())

    def test_breakpoint_hit_and_point_list(self):
        cl = self.connect()
        pid = cl.breakpoint_add(CODE9 + 4)
        self.assertGreaterEqual(pid, 0)
        ev = self.run_to_break(cl)
        self.assertEqual((ev.what, ev.pc, ev.cpu, ev.id), ("exec", CODE9 + 4, 9, pid))
        pts = cl.point_list()
        self.assertEqual(len(pts), 1)
        p = pts[0]
        self.assertEqual((p["id"], p["kind"], p["cpu"], p["enabled"]), (pid, "exec", 9, True))
        self.assertEqual((p["start"], p["end"], p["hits"]), (CODE9 + 4, CODE9 + 4, 1))

    def test_watchpoint_on_write(self):
        cl = self.connect()
        pid = cl.watchpoint_add(COUNTER, 4, read=False, write=True)
        ev = self.run_to_break(cl)
        self.assertEqual((ev.what, ev.addr, ev.id), ("write", COUNTER, pid))
        p = cl.point_list()[0]
        self.assertEqual((p["kind"], p["write"], p["read"], p["start"], p["end"]),
                         ("watch", True, False, COUNTER, COUNTER + 3))
        self.assertEqual(p["hits"], 1)

    def test_point_enable_disables(self):
        cl = self.connect()
        pid = cl.breakpoint_add(CODE9 + 4)
        self.run_to_break(cl)
        cl.point_enable(pid, False)
        self.assertFalse(cl.point_list()[0]["enabled"])
        cl.run(3)          # resumes from the park; the disabled point no longer stops it
        self.assertEqual(cl.point_list()[0]["hits"], 1)
        self.assertEqual(self.c.state, native.IDLE)
        cl.point_enable(pid, True)
        self.assertTrue(cl.point_list()[0]["enabled"])
        ev = self.run_to_break(cl)
        self.assertEqual(ev.id, pid)
        self.assertEqual(cl.point_list()[0]["hits"], 2)

    def test_point_remove(self):
        cl = self.connect()
        a = cl.breakpoint_add(CODE9 + 4)
        b = cl.watchpoint_add(COUNTER, 4)
        self.assertNotEqual(a, b)
        cl.point_remove(a)
        self.assertEqual([p["id"] for p in cl.point_list()], [b])
        self.expect_error("failed", cl.point_remove, a)
        self.expect_error("failed", cl.point_enable, a, True)

    def test_bad_arguments(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.request, T.breakpoint_add, struct.pack("<II", 0, CODE9))
        self.expect_error("bad_payload", cl.watchpoint_add, COUNTER, 0)
        self.expect_error("bad_payload", cl.watchpoint_add, COUNTER, 4, read=False, write=False)

    def test_other_console_points_are_separate(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.breakpoint_add(CODE9 + 4)
        self.assertEqual(cl.point_list(console=c2.id), [])
        self.assertEqual(len(cl.point_list(console=self.c.id)), 1)

    def test_running_console_errors(self):
        cl = self.connect()
        pid = cl.breakpoint_add(CODE9 + 0x40)       # never reached: the loop is 4 instructions
        cl.live_start(0)
        try:
            for name, fn in [
                ("breakpoint_add", lambda: cl.breakpoint_add(CODE9)),
                ("watchpoint_add", lambda: cl.watchpoint_add(COUNTER, 4)),
                ("point_remove", lambda: cl.point_remove(pid)),
                ("point_list", cl.point_list),
                ("point_enable", lambda: cl.point_enable(pid, False)),
                ("trace_config", lambda: cl.trace_config(10)),
                ("trace_read", cl.trace_read),
                ("calls_track", cl.calls_track),
                ("callstack", cl.callstack),
                ("call_counts_read", cl.call_counts_read),
                ("coverage_track", cl.coverage_track),
                ("coverage_read", cl.coverage_read),
                ("writers_track", cl.writers_track),
                ("last_writer", lambda: cl.last_writer(DATA)),
                ("lua_exec", lambda: cl.lua_exec("x = 1")),
                ("lua_run", lambda: cl.lua_run("x = 1")),
                ("lua_stop", cl.lua_stop),
            ]:
                with self.subTest(name):
                    self.expect_error("console_running", fn)
        finally:
            cl.live_stop()


class SavestateTests(DebugBase):
    def setUp(self):
        super().setUp()
        self.start_at(counter_program())

    def test_save_modify_load_restores(self):
        cl = self.connect()
        cl.run(3)
        target = DATA + 0x400
        self.c.write(target, b"\x11\x22\x33\x44")
        state = self.path("a.state")
        cl.savestate_save(state)
        self.assertTrue(os.path.getsize(state) > 1000)
        frames = self.c.frames
        self.c.write(target, b"\xAA\xBB\xCC\xDD")
        cl.run(2)
        cl.savestate_load(state)
        self.assertEqual(cl.read(target, 4), b"\x11\x22\x33\x44")
        self.assertEqual(self.c.frames, frames + 2)      # the frame counter is not part of the state

    def test_load_clears_input_queue_and_sends_event(self):
        cl = self.connect()
        state = self.path("b.state")
        cl.savestate_save(state)
        for _ in range(4):
            cl.input(buttons=["A"])
        self.assertEqual(self.c.input_depth(), 4)
        cl.take_events("input_queue")
        cl.savestate_load(state)
        ev = cl.next_event("input_queue")
        self.assertIsNotNone(ev)
        self.assertEqual((ev.console, ev.depth), (self.c.id, 0))
        self.assertEqual(self.c.input_depth(), 0)

    def test_errors(self):
        cl = self.connect()
        self.expect_error("failed", cl.savestate_load, self.path("missing.state"))
        self.expect_error("failed", cl.savestate_save, self.path("no-dir") + "/x/y.state")
        bad = self.path("bad.state")
        with open(bad, "wb") as f:
            f.write(b"not a savestate" * 10)
        self.expect_error("failed", cl.savestate_load, bad)

    def test_parked_console_gets_console_running(self):
        cl = self.connect()
        cl.breakpoint_add(CODE9 + 4)
        self.run_to_break(cl)
        for fn in (lambda: cl.savestate_save(self.path("p.state")), lambda: cl.savestate_load(self.path("p.state"))):
            err = self.expect_error("console_running", fn)
            self.assertIn("frame boundary", err.text)

    def test_running_console_gets_console_running(self):
        cl = self.connect()
        cl.live_start(0)
        try:
            self.expect_error("console_running", cl.savestate_save, self.path("r.state"))
        finally:
            cl.live_stop()


class LuaTests(DebugBase):
    def test_exec_output_event_carries_console_id(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.lua_exec("print('from zero')", console=self.c.id)
        cl.lua_exec("print('from one')", console=c2.id)
        evs = []
        while len(evs) < 2:
            ev = cl.next_event("lua_output", timeout=5)
            self.assertIsNotNone(ev)
            evs.append(ev)
        self.assertEqual(sorted((e.console, e.text) for e in evs),
                         sorted([(self.c.id, "from zero\n"), (c2.id, "from one\n")]))

    def test_run_script_prints_per_frame(self):
        cl = self.connect()
        cl.lua_run("for i = 1, 3 do print('f' .. emu.framecount()) emu.frameadvance() end")
        cl.run(5)
        text = cl.lua_text(self.c.id)
        self.assertEqual(text, "f0\nf1\nf2\n")

    def test_lua_file(self):
        script = self.path("s.lua")
        with open(script, "w") as f:
            f.write("print('file ' .. 6 * 7)")
        cl = self.connect()
        cl.lua_run(script, is_file=True)
        self.assertEqual(cl.next_event("lua_output", timeout=5).text, "file 42\n")

    def test_errors_and_stop(self):
        cl = self.connect()
        err = self.expect_error("failed", cl.lua_exec, "error('boom')")
        self.assertIn("boom", err.text)
        self.expect_error("failed", cl.lua_run, "this is not lua")
        cl.lua_run("while true do emu.frameadvance() print('tick') end")
        cl.run(2)
        cl.lua_stop()
        cl.take_events("lua_output")
        cl.run(2)
        self.assertEqual(cl.lua_text(self.c.id), "")

    def test_exec_while_parked(self):
        self.start_at(counter_program())
        cl = self.connect()
        cl.breakpoint_add(CODE9 + 4)
        self.run_to_break(cl)
        cl.lua_exec("print(memory.read_u32_le(0x02200100, 'Main RAM') ~= nil)")
        self.assertEqual(cl.next_event("lua_output", timeout=5).text, "true\n")

    def test_in_process_output_without_client(self):
        # The C API keeps working while the server is up but nobody is connected.
        self.assertEqual(self.c.lua_exec("print('direct')"), "direct\n")
        cl = self.connect()
        cl.lua_exec("print('socket')")
        self.assertEqual(cl.next_event("lua_output", timeout=5).text, "socket\n")
        cl.close()
        self.clients.remove(cl)
        time.sleep(0.1)
        self.assertEqual(self.c.lua_exec("print('direct again')"), "direct again\n")


class TraceTests(DebugBase):
    def setUp(self):
        super().setUp()
        self.code, self.labels = loop_program()
        self.start_at(self.code)
        self.words = words(self.code, CODE9)
        # the instruction a break stops at is traced (fetched) but not executed, so `done` is the last entry
        self.expected = ([CODE9, CODE9 + 4] + [self.labels["loop"], self.labels["loop"] + 4, self.labels["loop"] + 8] * 3
                         + [self.labels["done"]])

    def trace_run(self, cl):
        cl.trace_config(1000, cpu=9, start=CODE9, end=CODE9 + 0x40)
        cl.breakpoint_add(self.labels["done"])
        self.run_to_break(cl)

    def test_trace_pcs_and_opcodes_in_order(self):
        cl = self.connect()
        self.trace_run(cl)
        entries, truncated = cl.trace_read()
        self.assertFalse(truncated)
        self.assertEqual([e["pc"] for e in entries], self.expected)
        self.assertEqual([e["opcode"] for e in entries], [self.words[pc] for pc in self.expected])
        self.assertTrue(all(e["cpu"] == 9 for e in entries))
        cycles = [e["cycle"] for e in entries]
        self.assertEqual(cycles, sorted(cycles))
        # clearing
        self.assertEqual(cl.trace_read(), ([], False))

    def test_trace_truncated_and_rest_on_next_read(self):
        cl = self.connect()
        self.trace_run(cl)
        first, trunc1 = cl.trace_read(max=4)
        self.assertTrue(trunc1)
        self.assertEqual(len(first), 4)
        rest, trunc2 = cl.trace_read(max=100)
        self.assertFalse(trunc2)
        self.assertEqual([e["pc"] for e in first + rest], self.expected)

    def test_trace_off(self):
        cl = self.connect()
        cl.trace_config(1000, cpu=9, start=CODE9, end=CODE9 + 0x40)
        cl.trace_config(0)
        cl.breakpoint_add(self.labels["done"])
        self.run_to_break(cl)
        self.assertEqual(cl.trace_read(), ([], False))

    def test_coverage(self):
        cl = self.connect()
        cl.coverage_track(True)
        cl.breakpoint_add(self.labels["done"])
        self.run_to_break(cl)
        addrs, truncated = cl.coverage_read()
        self.assertFalse(truncated)
        ours = [a for a in addrs if CODE9 <= a < CODE9 + 0x40]
        self.assertEqual(ours, sorted(set(self.expected)))
        self.assertEqual([a for a in cl.coverage_read()[0] if CODE9 <= a < CODE9 + 0x40], [])

    def test_coverage_truncated_keeps_rest(self):
        cl = self.connect()
        cl.coverage_track(True)
        cl.breakpoint_add(self.labels["done"])
        self.run_to_break(cl)
        first, t1 = cl.coverage_read(max=2)
        self.assertTrue(t1)
        self.assertEqual(len(first), 2)
        rest = []
        while True:
            more, t = cl.coverage_read(max=3)
            rest += more
            if not t:
                break
        got = first + rest
        self.assertEqual(got, sorted(got))
        self.assertTrue(set(self.expected) <= set(got))


class CallTests(DebugBase):
    def setUp(self):
        super().setUp()
        self.code, self.l = call_program()
        self.start_at(self.code)

    def test_callstack_inside_nested_call(self):
        cl = self.connect()
        cl.calls_track(True)
        cl.breakpoint_add(self.l["f2"])
        ev = self.run_to_break(cl)
        self.assertEqual(ev.pc, self.l["f2"])
        frames = cl.callstack()
        self.assertEqual([f["entry"] for f in frames], [self.l["f2"], self.l["f1"]])
        self.assertEqual([f["site"] for f in frames], [self.l["f1_call"], self.l["main"]])
        self.assertEqual([f["ret"] for f in frames], [self.l["f1_call"] + 4, self.l["second"]])
        self.assertFalse(any(f["exception"] for f in frames))

    def test_call_counts(self):
        cl = self.connect()
        cl.calls_track(True)
        cl.breakpoint_add(self.l["done"])
        self.run_to_break(cl)
        counts, truncated = cl.call_counts_read()
        self.assertFalse(truncated)
        by_entry = {c["entry"]: c["count"] for c in counts}
        self.assertEqual(by_entry.get(self.l["f1"]), 2)
        self.assertEqual(by_entry.get(self.l["f2"]), 2)
        self.assertEqual(cl.call_counts_read(), ([], False))

    def test_call_counts_truncated_keeps_rest(self):
        cl = self.connect()
        cl.calls_track(True)
        cl.breakpoint_add(self.l["done"])
        self.run_to_break(cl)
        first, t1 = cl.call_counts_read(max=1)
        self.assertTrue(t1)
        self.assertEqual(len(first), 1)
        rest, t2 = cl.call_counts_read(max=10)
        self.assertFalse(t2)
        got = {c["entry"]: c["count"] for c in first + rest}
        self.assertEqual((got[self.l["f1"]], got[self.l["f2"]]), (2, 2))

    def test_callstack_empty_when_not_tracking(self):
        cl = self.connect()
        cl.breakpoint_add(self.l["f2"])
        self.run_to_break(cl)
        self.assertEqual(cl.callstack(), [])


class WriterTests(DebugBase):
    def test_last_writer_cpu(self):
        target = DATA + 0x300
        a = A.Asm(CODE9)
        a.ldr_const(0, target)
        a.mov(1, 5)
        store = a.pc
        a.str(1, 0)
        a.label("done")
        a.b("done")
        self.start_at(a.assemble())
        cl = self.connect()
        cl.writers_track(True)
        cl.breakpoint_add(a.labels["done"])
        self.run_to_break(cl)
        w = cl.last_writer(target)
        self.assertEqual((w["kind"], w["cpu"], w["pc"]), ("cpu", 9, store))
        self.assertIsNone(cl.last_writer(target + 0x40))

    def test_last_writer_arm7(self):
        target = DATA + 0x340
        a = A.Asm(CODE7)
        a.ldr_const(0, target)
        a.mov(1, 5)
        store = a.pc
        a.str(1, 0)
        a.label("done")
        a.b("done")
        self.start_at(a.assemble(), CODE7, cpu=7)
        cl = self.connect()
        cl.writers_track(True)
        cl.breakpoint_add(a.labels["done"], cpu=7)
        self.run_to_break(cl)
        w = cl.last_writer(target)
        self.assertEqual((w["kind"], w["cpu"], w["pc"]), ("cpu", 7, store))

    def test_last_writer_dma(self):
        src, dst = DATA + 0x500, DATA + 0x600
        self.c.write(src, b"\x01\x02\x03\x04" * 4)
        a = A.Asm(CODE9)
        a.ldr_const(0, 0x040000B0)       # DMA0 SAD
        a.ldr_const(1, src)
        a.str(1, 0, 0)
        a.ldr_const(1, dst)
        a.str(1, 0, 4)
        a.ldr_const(1, 0x84000004)       # enable, 32-bit, immediate, 4 units
        a.str(1, 0, 8)
        a.label("done")
        a.b("done")
        self.start_at(a.assemble())
        cl = self.connect()
        cl.writers_track(True)
        cl.run(3)
        self.assertEqual(self.c.read(dst, 4), b"\x01\x02\x03\x04")
        w = cl.last_writer(dst)
        self.assertIsNotNone(w)
        self.assertEqual((w["kind"], w["cpu"], w["channel"]), ("dma", 9, 0))

    def test_last_writer_none_when_tracking_off(self):
        self.start_at(counter_program())
        cl = self.connect()
        cl.run(2)
        self.assertIsNone(cl.last_writer(COUNTER))


class PacketTests(DebugBase):
    def test_packets_track_and_read_while_running(self):
        self.start_at(counter_program())
        cl = self.connect()
        cl.live_start(0)
        try:
            cl.packets_track(True, 64)
            self.wait_until(lambda: self.c.frames > 3, what="frames")
            self.assertEqual(cl.packets_read(), ([], False))
            self.assertEqual(self.c.state, native.RUNNING)
            cl.packets_track(False)
        finally:
            cl.live_stop()

    def test_packets_read_while_idle(self):
        cl = self.connect()
        cl.packets_track(True, 8)
        cl.run(2)
        self.assertEqual(cl.packets_read(max=5), ([], False))


class ProcessTests(DebugBase):
    def test_pacing_slows_and_unlimited_speeds(self):
        self.start_at(counter_program())
        cl = self.connect()
        cl.pacing(20)
        t0 = time.time()
        cl.run(8)
        slow = time.time() - t0
        self.assertGreater(slow, 0.3)
        cl.pacing(0)
        t0 = time.time()
        cl.run(8)
        self.assertLess(time.time() - t0, slow)

    def test_mp_timeout(self):
        cl = self.connect()
        before = native.mp_timeout(-1)
        try:
            cl.mp_timeout(40)
            self.assertEqual(native.mp_timeout(-1), 40)
        finally:
            native.mp_timeout(before)

    def test_console_field_ignored(self):
        cl = self.connect()
        cl.request(T.pacing, struct.pack("<I", 0), console=99)
        cl.request(T.mp_timeout, struct.pack("<I", native.mp_timeout(-1)), console=99)


class InputLogTests(DebugBase):
    def test_log_carries_full_payloads(self):
        self.start_at(counter_program())
        cl = self.connect()
        cl.input(buttons=["A", "Up"], touch=[(0, 10, 20, 1), (1000, 11, 21, 0)], mic=[1, -2, 3], seq=77)
        cl.input(buttons=["B"], lid=True, mic_level=900, seq=78)
        cl.input(buttons=0, seq=79)
        cl.run(3)
        entries, truncated = cl.input_log_read(0, 10)
        self.assertFalse(truncated)
        self.assertEqual([e["seq"] for e in entries], [77, 78, 79])
        self.assertEqual([e["frame"] for e in entries], [0, 1, 2])
        first, second, third = entries
        self.assertEqual(first["buttons"], 0b1000001)           # A (bit 0) and Up (bit 6)
        self.assertEqual(first["touch"], [(0, 10, 20, 1), (1000, 11, 21, 0)])
        self.assertEqual((first["mic_kind"], first["mic"]), (1, [1, -2, 3]))
        self.assertEqual((second["buttons"], second["lid"], second["mic_kind"], second["mic_level"]),
                         (0b10, 1, 2, 900))
        self.assertEqual(second["touch"], [])
        self.assertEqual((third["buttons"], third["mic_kind"], third["mic"]), (0, 0, []))

    def test_truncated_and_from_frame(self):
        self.start_at(counter_program())
        cl = self.connect()
        for i in range(5):
            cl.input(buttons=0, seq=100 + i)
        cl.run(5)
        part, truncated = cl.input_log_read(0, 2)
        self.assertTrue(truncated)
        self.assertEqual([e["seq"] for e in part], [100, 101])
        tail, truncated = cl.input_log_read(3, 10)
        self.assertFalse(truncated)
        self.assertEqual([e["seq"] for e in tail], [103, 104])

    def test_local_sources_have_seq_zero(self):
        self.start_at(counter_program())
        cl = self.connect()
        self.c.set_keys(["Start"])
        self.c.touch(30, 40)
        cl.run(1)
        entries, _ = cl.input_log_read(0, 10)
        self.assertEqual(len(entries), 1)
        self.assertEqual((entries[0]["seq"], entries[0]["buttons"], entries[0]["touch"]),
                         (0, 1 << 3, [(0, 30, 40, 1)]))


class SymbolTests(DebugBase):
    def setUp(self):
        super().setUp()
        self.code, self.l = call_program()
        self.start_at(self.code)
        self.sym = self.path("game.sym")
        names = {"main": 8, "f1": 8, "f2": 12}
        with open(self.sym, "w") as f:
            f.write(f"{self.l['main']:08x} main_loop 8\n")
            f.write(f"{self.l['done']:08x} done_spin 4\n")
            f.write(f"{self.l['f1']:08x} func_one 8\n")
            f.write(f"{self.l['f2']:08x} func_two 12\n")
            f.write(f"{DATA + 0x400:08x} data_buf 0x40\n")
        self.cl = self.connect()

    def test_symbols_load_counts_and_errors(self):
        n, modules = self.cl.symbols_load(self.sym)
        self.assertEqual((n, modules), (5, 1))
        self.expect_error("failed", self.cl.symbols_load, self.path("missing.sym"))
        self.assertEqual(self.cl.symbols_load(""), (0, 0))

    def test_trace_names(self):
        cl = self.cl
        cl.symbols_load(self.sym)
        cl.trace_config(100, cpu=9, start=CODE9, end=CODE9 + 0x80)
        cl.breakpoint_add(self.l["done"])
        self.run_to_break(cl)
        entries, _ = cl.trace_read(with_symbols=True)
        by_pc = {e["pc"]: e["name"] for e in entries}
        self.assertEqual(by_pc[self.l["main"]], "main_loop")
        self.assertEqual(by_pc[self.l["f1"]], "func_one")
        self.assertEqual(by_pc[self.l["f1"] + 4], "func_one+0x4")
        self.assertEqual(by_pc[self.l["f2"] + 8], "func_two+0x8")

    def test_callstack_names(self):
        cl = self.cl
        cl.symbols_load(self.sym)
        cl.calls_track(True)
        cl.breakpoint_add(self.l["f2"])
        self.run_to_break(cl)
        frames = cl.callstack(with_symbols=True)
        self.assertEqual([f["name"] for f in frames], ["func_two", "func_one"])
        self.assertEqual([f["site_name"] for f in frames], ["func_one+0x4", "main_loop"])

    def test_call_counts_and_coverage_names(self):
        cl = self.cl
        cl.symbols_load(self.sym)
        cl.calls_track(True)
        cl.coverage_track(True)
        cl.breakpoint_add(self.l["done"])
        self.run_to_break(cl)
        counts, _ = cl.call_counts_read(with_symbols=True)
        self.assertEqual({c["name"]: c["count"] for c in counts if c["name"]}, {"func_one": 2, "func_two": 2})
        cov, _ = cl.coverage_read(with_symbols=True)
        named = {a: n for a, n in cov if n}
        self.assertEqual(named[self.l["f2"]], "func_two")
        self.assertEqual(named[self.l["f2"] + 4], "func_two+0x4")
        self.assertEqual(named[self.l["done"]], "done_spin")       # fetched, then the break stopped it

    def test_search_names(self):
        cl = self.cl
        cl.symbols_load(self.sym)
        self.c.write(DATA + 0x410, struct.pack("<I", 0xCAFEF00D))
        self.c.write(DATA + 0x420, b"zebra\0")
        hits, _ = cl.memory_search(0xCAFEF00D, size=4, start=DATA, length=0x1000, with_symbols=True)
        self.assertEqual(hits, [(DATA + 0x410, "data_buf+0x10")])
        hits, _ = cl.text_find("zebra", start=DATA, length=0x1000, with_symbols=True)
        self.assertEqual(hits, [((DATA + 0x420, 1), "data_buf+0x20")])

    def test_without_symbols_loaded_names_are_empty(self):
        cl = self.cl
        cl.coverage_track(True)
        cl.breakpoint_add(self.l["done"])
        self.run_to_break(cl)
        cov, _ = cl.coverage_read(with_symbols=True)
        self.assertTrue(cov)
        self.assertTrue(all(n == "" for _, n in cov))

    def test_old_clients_without_the_flag_get_old_layout(self):
        cl = self.cl
        body = cl.request(T.coverage_read, struct.pack("<BI", 9, 0)).body
        self.assertEqual(body, struct.pack("<BI", 0, 0))          # truncated, count 0, no names section
        body = cl.request(T.coverage_read, struct.pack("<BIB", 9, 0, 1)).body
        self.assertEqual(body, struct.pack("<BII", 0, 0, 0))      # with the flag: an empty names section

    def test_tables_are_per_console(self):
        c2 = self.new_console()
        self.cl.symbols_load(self.sym)
        self.c.write(DATA + 0x410, struct.pack("<I", 0xCAFEF00D))
        c2.write(DATA + 0x410, struct.pack("<I", 0xCAFEF00D))
        hits, _ = self.cl.memory_search(0xCAFEF00D, size=4, start=DATA, length=0x1000, with_symbols=True,
                                        console=c2.id)
        self.assertEqual(hits, [(DATA + 0x410, "")])
