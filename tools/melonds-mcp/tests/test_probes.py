"""API level 7 (feature/probes): function probes, the memory access log, breakpoint actions and overlay tags,
none of which parks the console (docs/headless-api.md, section 5b)."""
import ctypes
import unittest

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE9, DATA, ConsoleTest

RESULTS = DATA + 0x40


def caller(calls=10, forever=False):
    """r4 counts calls of f(r4, DATA) and each result is stored at RESULTS + 4 * r4; f returns r0 + 100, r1 = 7
    and writes r0 + 1 to [r1 + 0x80]. With forever, the loop never ends (r4 keeps counting; results all go to
    RESULTS)."""
    a = A.Asm(CODE9)
    a.mov(4, 0)
    a.ldr_const(5, DATA)
    a.ldr_const(6, RESULTS)
    a.label("loop")
    a.mov_reg(0, 4)
    a.mov_reg(1, 5)
    a.bl("f")
    a.label("back")
    a.str(0, 6, 0)
    if not forever:
        a.add(6, 6, 4)
    a.add(4, 4, 1)
    if forever:
        a.b("loop")
    else:
        a.cmp(4, calls)
        a.b("loop", cond=A.LT)
    a.label("spin")
    a.b("spin")
    a.label("f")
    a.add(2, 0, 1)
    a.str(2, 1, 0x80)
    a.add(0, 0, 100)
    a.mov(1, 7)
    a.bx(14)
    return a


class Level7Features(unittest.TestCase):
    def test_features(self):
        for name in ("probes", "memlog", "bp_actions", "overlay_tags", "mds_probe_add", "mds_probe_take",
                     "mds_memlog_take", "mds_action_add", "mds_overlay_at", "mds_callcounts_take_ov",
                     "mds_coverage_take_ov"):
            with self.subTest(name):
                self.assertTrue(native.has(name))

    def test_struct_sizes(self):
        self.assertEqual(ctypes.sizeof(native.MdsProbeRecord), 128)
        self.assertEqual(ctypes.sizeof(native.MdsMemlogRecord), 48)
        self.assertEqual(ctypes.sizeof(native.MdsActionRecord), 80)
        self.assertEqual(ctypes.sizeof(native.MdsProbeSpec), 84)
        self.assertEqual(ctypes.sizeof(native.MdsActionSpec), 48)


class Probes(ConsoleTest):
    def setUp(self):
        super().setUp()
        self.prog = caller()
        self.load_program(self.prog)
        self.f = self.prog.labels["f"]
        self.back = self.prog.labels["back"]
        self.c.write(DATA, bytes(range(16)))

    def test_calls_arguments_and_returns(self):
        p = self.c.add_probe(self.f, reads=[("r1", 8), (1, 4, 0x80, "both")])
        self.run_frames(1)
        recs = self.c.take_probes()
        self.assertEqual(len(recs), 10)
        for i, r in enumerate(recs):
            self.assertEqual((r["seq"], r["probe"], r["cpu"], r["addr"]), (i, p, 9, self.f))
            self.assertEqual(r["args"][:2], [i, DATA])
            self.assertEqual(r["ret"], [i + 100, 7])
            self.assertEqual(r["lr"], self.back)
            self.assertEqual(r["reads"][0], bytes(range(8)))
            # [r1 + 0x80] before the call (the previous call's write), and after it
            self.assertEqual(r["reads"][1], i.to_bytes(4, "little"))
            self.assertEqual(r["ret_reads"][1], (i + 1).to_bytes(4, "little"))
            self.assertEqual((r["depth"], r["overlay"], r["frame"], r["forced"], r["lost"]), (-1, -1, 0, False, False))
        self.assertEqual(self.c.probes()[0]["hits"], 10)
        self.assertEqual(self.c.probe_stats(), {"recorded": 10, "dropped": 0, "pending": 0, "in_ring": 0})

    def test_nothing_parks_and_the_run_is_unchanged(self):
        self.c.add_probe(self.f, reads=[("r1", 8)])
        self.run_frames(2)
        self.assertEqual([self.word(RESULTS + 4 * i) for i in range(10)], list(range(100, 110)))

    def test_depth_with_call_tracking(self):
        self.c.track_calls(True)
        self.c.add_probe(self.f)
        self.run_frames(1)
        depths = {r["depth"] for r in self.c.take_probes()}
        self.assertEqual(depths, {1})

    def test_lr_filter(self):
        self.c.add_probe(self.f, lr_range=(self.back + 4, self.back + 0x100))
        self.run_frames(1)
        self.assertEqual(self.c.take_probes(), [])
        self.c.add_probe(self.f, lr_range=(self.back, self.back))
        self.load_program(self.prog)
        self.run_frames(1)
        self.assertEqual(len(self.c.take_probes()), 10)

    def test_no_return(self):
        self.c.add_probe(self.f, returns=False)
        self.run_frames(1)
        recs = self.c.take_probes()
        self.assertEqual(len(recs), 10)
        self.assertTrue(all(not r["returned"] and r["ret"] is None for r in recs))
        self.assertEqual(self.c.probe_stats()["pending"], 0)

    def test_ring_drops_the_oldest(self):
        self.c.probe_capacity(4)
        self.c.add_probe(self.f)
        self.run_frames(1)
        recs = self.c.take_probes()
        self.assertEqual([r["seq"] for r in recs], [6, 7, 8, 9])
        self.assertEqual(self.c.probe_stats()["dropped"], 6)

    def test_remove(self):
        p = self.c.add_probe(self.f)
        self.c.remove_probe(p)
        self.assertEqual(self.c.probes(), [])
        with self.assertRaises(MelonError):
            self.c.remove_probe(p)
        self.run_frames(1)
        self.assertEqual(self.c.take_probes(), [])

    def test_bad_reads_fail(self):
        with self.assertRaises(MelonError) as cm:
            self.c.add_probe(self.f, reads=[("r1", 257)])
        self.assertIn("at most 256", str(cm.exception))
        with self.assertRaises(MelonError):
            self.c.add_probe(self.f, reads=[(16, 4)])

    def test_needs_a_stopped_console_with_the_debugger(self):
        d = native.Console(debug=False)
        d.reset()
        self.addCleanup(d.close)
        with self.assertRaises(MelonError) as cm:
            d.add_probe(self.f)
        self.assertIn("without the debugger", str(cm.exception))
        self.c.start(60)
        try:
            with self.assertRaises(MelonError) as cm:
                self.c.add_probe(self.f)
            self.assertIn("is running", str(cm.exception))
        finally:
            self.c.stop_after(0)
            self.c.wait()


class LateReturn(ConsoleTest):
    def test_a_call_taken_before_it_returns_gets_its_return_later(self):
        a = A.Asm(CODE9)
        a.mov(0, 3)
        a.bl("f")
        a.label("back")
        a.b("back")
        a.label("f")
        a.spin(2, 0x40000)      # a few frames
        a.mov(0, 5)
        a.bx(14)
        self.load_program(a)
        self.c.add_probe(a.labels["f"])
        self.run_frames(1)
        pending = {}
        first = self.c.take_probes(pending)
        self.assertEqual(len(first), 1)
        self.assertFalse(first[0]["returned"])
        self.assertEqual(self.c.probe_stats()["pending"], 1)
        self.run_frames(30)
        self.assertEqual(self.c.take_probes(pending), [])    # merged into the earlier record
        self.assertTrue(first[0]["returned"])
        self.assertEqual(first[0]["ret"][0], 5)
        self.assertGreater(first[0]["ret_frame"], first[0]["frame"])
        self.assertEqual(pending, {})

    def test_without_pending_the_late_return_is_its_own_record(self):
        a = A.Asm(CODE9)
        a.bl("f")
        a.label("back")
        a.b("back")
        a.label("f")
        a.spin(2, 0x40000)
        a.mov(0, 9)
        a.bx(14)
        self.load_program(a)
        self.c.add_probe(a.labels["f"])
        self.run_frames(1)
        self.assertFalse(self.c.take_probes()[0]["returned"])
        self.run_frames(30)
        (late,) = self.c.take_probes()
        self.assertEqual((late["late"], late["seq"], late["ret"][0], late["addr"]), (True, 0, 9, a.labels["f"]))

    def test_a_state_load_gives_up_the_returns_awaited(self):
        a = A.Asm(CODE9)
        a.bl("f")
        a.label("back")
        a.b("back")
        a.label("f")
        a.spin(2, 0x100000)
        a.bx(14)
        self.load_program(a)
        import tempfile, os
        path = os.path.join(tempfile.mkdtemp(), "s.mln")
        self.c.save_state(path)
        self.c.add_probe(a.labels["f"])
        self.run_frames(1)
        self.c.load_state(path)
        recs = self.c.take_probes()
        self.assertEqual(len(recs), 1)
        self.assertTrue(recs[0]["lost"])
        self.assertEqual(self.c.probe_stats()["pending"], 0)


class Determinism(unittest.TestCase):
    """A run with probes and the memory log is the same run as one without: same counter after the same frames."""

    def run_one(self, setup):
        c = native.Console()
        try:
            c.reset()
            prog = caller(forever=True)
            c.write(CODE9, prog.assemble())
            c.set_reg("pc", CODE9)
            setup(c, prog)
            c.run(3)
            return c.regs()["r4"], c.read(DATA, 0x100), c.regs()["pc"]
        finally:
            c.close()

    def test_same_run(self):
        plain = self.run_one(lambda c, p: None)
        probed = self.run_one(lambda c, p: c.add_probe(p.labels["f"], reads=[("r1", 64, 0, "both")]))
        logged = self.run_one(lambda c, p: c.memlog_add(DATA, 0x100))
        both = self.run_one(lambda c, p: (c.add_probe(p.labels["f"]), c.memlog_add(DATA, 0x100), c.overlay_tags(True),
                                          c.track_calls(True)))
        self.assertGreater(plain[0], 1000)
        self.assertEqual(probed, plain)
        self.assertEqual(logged, plain)
        self.assertEqual(both, plain)


class MemLog(ConsoleTest):
    def test_reads_and_writes(self):
        a = A.Asm(CODE9)
        a.ldr_const(1, DATA)
        a.ldr_const(2, 0x11223344)
        a.label("w")
        a.str(2, 1, 8)
        a.label("r")
        a.ldrh(3, 1, 10)
        a.strb(3, 1, 0x20)      # outside the range
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        rng = self.c.memlog_add(DATA, 0x10)
        self.run_frames(1)
        log = self.c.take_memlog()
        self.assertEqual([(x["write"], x["addr"], x["size"], x["value"], x["pc"], x["dma"], x["range"]) for x in log],
                         [(True, DATA + 8, 4, 0x11223344, a.labels["w"], None, rng),
                          (False, DATA + 10, 2, 0x1122, a.labels["r"], None, rng)])
        self.assertEqual(self.c.memlog_stats(), {"recorded": 2, "dropped": 0, "in_ring": 0})
        self.assertEqual(self.c.memlog_ranges()[0]["hits"], 2)

    def test_pc_filter_and_kinds(self):
        a = A.Asm(CODE9)
        a.ldr_const(1, DATA)
        a.str(1, 1, 0)
        a.label("second")
        a.str(1, 1, 4)
        a.ldr(2, 1, 4)
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.c.memlog_add(DATA, 8, read=False, pc_range=(a.labels["second"], a.labels["second"]))
        self.run_frames(1)
        log = self.c.take_memlog()
        self.assertEqual([(x["addr"], x["write"]) for x in log], [(DATA + 4, True)])

    def test_dma_units(self):
        src, dst = DATA + 0x100, DATA + 0x200
        a = A.Asm(CODE9)
        a.ldr_const(0, 0x040000B0)
        a.ldr_const(1, src)
        a.str(1, 0, 0)
        a.ldr_const(1, dst)
        a.str(1, 0, 4)
        a.ldr_const(1, 0x84000002)     # enable, 32-bit, start at once, 2 units
        a.str(1, 0, 8)
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.c.write(src, bytes.fromhex("0102030405060708"))
        self.c.memlog_add(src, 8, write=False)
        self.c.memlog_add(dst, 8, read=False)
        self.run_frames(1)
        log = self.c.take_memlog()
        dma = [(x["write"], x["addr"], x["value"], x["dma"], x["pc"]) for x in log]
        self.assertEqual(dma, [(False, src, 0x04030201, 0, native.DMA_TAG), (True, dst, 0x04030201, 0, native.DMA_TAG),
                               (False, src + 4, 0x08070605, 0, native.DMA_TAG),
                               (True, dst + 4, 0x08070605, 0, native.DMA_TAG)])

    def test_dma_left_out_without_the_flag(self):
        src, dst = DATA + 0x100, DATA + 0x200
        a = A.Asm(CODE9)
        a.ldr_const(0, 0x040000B0)
        a.ldr_const(1, src)
        a.str(1, 0, 0)
        a.ldr_const(1, dst)
        a.str(1, 0, 4)
        a.ldr_const(1, 0x84000002)
        a.str(1, 0, 8)
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.c.memlog_add(dst, 8, dma=False)
        self.run_frames(1)
        self.assertEqual(self.c.take_memlog(), [])

    def test_ring_and_capacity(self):
        a = A.Asm(CODE9)
        a.ldr_const(1, DATA)
        for i in range(6):
            a.str(1, 1, 4 * i)
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.c.memlog_capacity(4)
        self.c.memlog_add(DATA, 0x20)
        self.run_frames(1)
        self.assertEqual([x["seq"] for x in self.c.take_memlog()], [2, 3, 4, 5])
        self.assertEqual(self.c.memlog_stats()["dropped"], 2)

    def test_bad_ranges_fail(self):
        with self.assertRaises(MelonError):
            self.c.memlog_add(DATA, 0)
        with self.assertRaises(MelonError):
            self.c.memlog_add(DATA, 4, read=False, write=False)


class Actions(ConsoleTest):
    def setUp(self):
        super().setUp()
        self.prog = caller()
        self.load_program(self.prog)
        self.f = self.prog.labels["f"]
        self.back = self.prog.labels["back"]

    def results(self, n=10):
        return [self.word(RESULTS + 4 * i) for i in range(n)]

    def test_return_early_with_a_value(self):
        act = self.c.add_action(self.f, "return", 0x1234, first=3, times=2)
        self.run_frames(1)
        res = self.results()
        self.assertEqual(res[:2], [100, 101])
        self.assertEqual(res[2:4], [0x1234, 0x1234])
        self.assertEqual(res[4:], [104, 105, 106, 107, 108, 109])
        log = self.c.take_actions()
        self.assertEqual([(x["action"], x["kind"], x["hit"], x["pc"], x["lr"], x["regs"][0], x["value"]) for x in log],
                         [(act, "return", 3, self.f, self.back, 2, 0x1234), (act, "return", 4, self.f, self.back, 3, 0x1234)])
        self.assertEqual(self.c.actions()[0]["matched"], 10)
        self.assertEqual(self.c.actions()[0]["fired"], 2)
        self.assertEqual(self.c.action_stats()["applied"], 2)
        # the body did not run: [r1 + 0x80] keeps the last real call's value
        self.assertEqual(self.word(DATA + 0x80), 10)

    def test_probe_sees_the_forced_return(self):
        self.c.add_probe(self.f)
        self.c.add_action(self.f, "return", 0x55, value2=0x66, times=1)
        self.run_frames(1)
        recs = self.c.take_probes()
        self.assertTrue(recs[0]["forced"])
        self.assertEqual(recs[0]["ret"], [0x55, 0x66])
        self.assertFalse(any(r["forced"] for r in recs[1:]))
        self.assertEqual(recs[1]["ret"], [101, 7])

    def test_lr_filter(self):
        self.c.add_action(self.f, "return", 1, lr=self.back + 4)
        self.run_frames(1)
        self.assertEqual(self.c.take_actions(), [])
        self.assertEqual(self.results()[:2], [100, 101])

    def test_set_reg_write_and_skip(self):
        # set r0 = 50 at f's entry: f returns 150 every time
        self.c.add_action(self.f, "set_reg", 50, reg="r0", times=1)
        # write 0xAB at DATA + 0x90 at the first call
        self.c.add_action(self.f, "write", 0xAB, target=DATA + 0x90, size=1, times=1)
        self.run_frames(1)
        self.assertEqual(self.results()[:2], [150, 101])
        self.assertEqual(self.c.read(DATA + 0x90, 1), b"\xab")
        self.assertEqual([x["kind"] for x in self.c.take_actions()], ["set_reg", "write"])

    def test_skip(self):
        # skip f's first two instructions (add r2, str): [r1 + 0x80] is never written
        self.c.add_action(self.f, "skip", size=2)
        self.run_frames(1)
        self.assertEqual(self.word(DATA + 0x80), 0)
        self.assertEqual(self.results()[:3], [100, 101, 102])

    def test_a_breakpoint_at_the_return_address_fires_after_a_return_action(self):
        self.c.add_action(self.f, "return", 9, times=1)
        bp = self.c.add_breakpoint(self.back)
        _, states = self.c.run(1)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.id, b.pc), ("exec", bp, self.back))
        self.assertEqual(self.c.regs()["r0"], 9)

    def test_bad_actions_fail(self):
        with self.assertRaises(MelonError):
            self.c.add_action(self.f, "write", 1, target=0x04000000, size=4)
        with self.assertRaises(MelonError):
            self.c.add_action(self.f, "write", 1, target=DATA, size=3)
        with self.assertRaises(MelonError):
            self.c.add_action(self.f, "set_reg", 1, reg=16)
        with self.assertRaises(MelonError):
            self.c.add_action(self.f, "skip", size=0)
        with self.assertRaises(MelonError):
            self.c.add_action(self.f, 9)

    def test_remove(self):
        act = self.c.add_action(self.f, "return", 1)
        self.c.remove_action(act)
        self.run_frames(1)
        self.assertEqual(self.results()[:2], [100, 101])


class OverlayTags(ConsoleTest):
    def test_without_a_cart_everything_is_static(self):
        prog = caller()
        self.load_program(prog)
        self.c.track_calls(True)
        self.c.coverage(True)
        self.c.overlay_tags(True)
        self.run_frames(1)
        tagged = self.c.take_call_counts_ov()
        plain = self.c.take_call_counts()
        self.assertEqual(tagged[(prog.labels["f"], native.OVERLAY_STATIC)], 10)
        self.assertEqual({e: n for (e, _), n in tagged.items()}, plain)
        cov = self.c.take_coverage_ov()
        self.assertEqual(sorted(a for a, _ in cov), self.c.take_coverage())
        self.assertEqual({o for _, o in cov}, {native.OVERLAY_STATIC})
        self.assertEqual(self.c.overlay_at(prog.labels["f"]), native.OVERLAY_STATIC)

    def test_untagged_takes_are_unchanged_with_tags_off(self):
        prog = caller()
        self.load_program(prog)
        self.c.track_calls(True)
        self.run_frames(1)
        self.assertEqual(self.c.take_call_counts_ov(), {})
        self.assertEqual(self.c.take_call_counts()[prog.labels["f"]], 10)
