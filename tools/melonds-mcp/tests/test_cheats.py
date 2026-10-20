"""Action Replay codes (API level 3, feature "cheats"): mds_cheat_add / remove / enable / clear / list / get.

The codes run in the core's AREngine, which the ARM7 enters when it takes a VBlank interrupt (the way an AR
cartridge hooks the ARM7's VBlank handler). FreeBIOS without a cart never enables interrupts, so these tests run
a small ARM7 program that enables the VBlank IRQ, acknowledges it and counts it: a code then runs once per frame.
"""
import os
import tempfile
import unittest

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest

IRQ_COUNT = DATA + 0x100        # the ARM7 handler counts VBlank IRQs here
HANDLER7 = CODE7 + 0x400
TARGET = DATA + 0x200           # what the test codes write
MSR_CPSR_C = 0xE321F000         # msr cpsr_c, #imm


def vblank_irq_program():
    """ARM7: IRQ and system stacks, the BIOS's IRQ handler pointer, DISPSTAT VBlank IRQ, IE, IME, then spin
    with interrupts on. Returns (main program, handler)."""
    a = A.Asm(CODE7)
    a.word(MSR_CPSR_C | 0xD2)            # IRQ mode, IRQs off
    a.ldr_const(13, 0x0380FD00)
    a.word(MSR_CPSR_C | 0xDF)            # system mode, IRQs off
    a.ldr_const(13, 0x0380FC00)
    a.ldr_const(0, 0x03FFFFFC)           # FreeBIOS calls [0x03FFFFFC] on an IRQ
    a.ldr_const(1, HANDLER7)
    a.str(1, 0, 0)
    a.ldr_const(0, 0x04000000)
    a.ldrh(1, 0, 4)
    a.orr(1, 1, 8)                       # DISPSTAT: VBlank IRQ enable
    a.strh(1, 0, 4)
    a.ldr_const(0, 0x04000210)
    a.mov(1, 1)
    a.str(1, 0, 0)                       # IE = VBlank
    a.ldr_const(1, 0xFFFFFFFF)
    a.str(1, 0, 4)                       # IF: clear everything pending
    a.ldr_const(0, 0x04000208)
    a.mov(1, 1)
    a.str(1, 0, 0)                       # IME = 1
    a.word(MSR_CPSR_C | 0x1F)            # system mode, IRQs on
    a.label("spin")
    a.b("spin")

    h = A.Asm(HANDLER7)
    h.ldr_const(0, 0x04000214)
    h.mov(1, 1)
    h.str(1, 0, 0)                       # acknowledge VBlank
    h.ldr_const(0, IRQ_COUNT)
    h.ldr(1, 0, 0)
    h.add(1, 1, 1)
    h.str(1, 0, 0)
    h.bx(14)
    return a, h


def calls_program():
    """ARM9: a loop calling f1 from "site" (a code can patch that call to f2). Returns (asm, site, f1, f2)."""
    a = A.Asm(CODE9)
    a.label("loop")
    a.label("site")
    a.bl("f1")
    a.b("loop")
    a.label("f1")
    a.add(2, 2, 1)
    a.bx(14)
    a.label("f2")
    a.add(3, 3, 1)
    a.bx(14)
    a.assemble()
    return a, a.labels["site"], a.labels["f1"], a.labels["f2"]


def bl(site, target):
    return 0xEB000000 | (((target - (site + 8)) >> 2) & 0xFFFFFF)


def write32(addr, value):
    return f"{addr & 0x0FFFFFFF:08X} {value:08X}"


# datareg = [TARGET]; datareg += 1; [TARGET] = datareg: counts the frames the code ran in
INCREMENT = "\n".join([f"D9000000 {TARGET:08X}", "D4000000 00000001", f"D6000000 {TARGET:08X}", "D2000000 00000000"])


class CheatTest(ConsoleTest):
    def setUp(self):
        super().setUp()
        self.c.write(DATA, bytes(0x400))
        main, handler = vblank_irq_program()
        self.c.write(HANDLER7, handler.assemble(), cpu=7)
        self.load_program(main, CODE7, cpu=7)

    def irqs(self):
        return self.word(IRQ_COUNT)


class Feature(unittest.TestCase):
    def test_api_level_and_feature(self):
        self.assertGreaterEqual(native.api_version(), 3)
        self.assertTrue(native.has("cheats"))
        for fn in ("mds_cheat_add", "mds_cheat_remove", "mds_cheat_enable", "mds_cheat_clear", "mds_cheat_list",
                   "mds_cheat_get"):
            self.assertTrue(native.has(fn), fn)


class Running(CheatTest):
    def test_program_takes_one_vblank_irq_per_frame(self):
        self.run_frames(3)
        before = self.irqs()
        self.run_frames(10)
        self.assertEqual(self.irqs() - before, 10)

    def test_writes_of_each_size(self):
        self.c.add_cheat("\n".join([write32(TARGET, 0x12345678), f"1{TARGET + 4 & 0x0FFFFFFF:07X} 0000BEEF",
                                    f"2{TARGET + 6 & 0x0FFFFFFF:07X} 000000AA"]), "writes")
        self.run_frames(2)
        self.assertEqual(self.c.read(TARGET, 8), bytes.fromhex("78563412efbeaa00"))

    def test_a_code_runs_once_per_frame(self):
        self.run_frames(2)
        self.c.add_cheat(INCREMENT, "count")
        start_irqs = self.irqs()
        self.run_frames(20)
        self.assertEqual(self.word(TARGET), self.irqs() - start_irqs)
        self.assertEqual(self.word(TARGET), 20)

    def test_codes_run_in_list_order(self):
        self.c.add_cheat(write32(TARGET, 1), "first")
        self.c.add_cheat(write32(TARGET, 2), "second")
        self.run_frames(2)
        self.assertEqual(self.word(TARGET), 2)

    def test_disabled_codes_do_not_run(self):
        cid = self.c.add_cheat(INCREMENT, "count", enabled=False)
        self.run_frames(5)
        self.assertEqual(self.word(TARGET), 0)
        self.c.enable_cheat(cid, True)
        self.run_frames(5)
        self.assertEqual(self.word(TARGET), 5)
        self.c.enable_cheat(cid, False)
        self.run_frames(5)
        self.assertEqual(self.word(TARGET), 5)

    def test_remove_and_clear(self):
        a = self.c.add_cheat(INCREMENT, "count")
        self.run_frames(3)
        self.c.remove_cheat(a)
        self.run_frames(3)
        self.assertEqual(self.word(TARGET), 3)
        self.c.add_cheat(INCREMENT)
        self.c.add_cheat(write32(TARGET + 8, 7))
        self.assertEqual(self.c.clear_cheats(), 2)
        self.assertEqual(self.c.cheats(), [])
        self.run_frames(3)
        self.assertEqual(self.word(TARGET), 3)
        self.assertEqual(self.c.clear_cheats(), 0)

    def test_key_condition(self):
        # 94000130 FFFE0000: IF A is pressed (KEYINPUT bit 0 low)
        self.c.add_cheat(f"94000130 FFFE0000\n{write32(TARGET, 0xA)}\nD2000000 00000000", "on A")
        self.run_frames(3)
        self.assertEqual(self.word(TARGET), 0)
        self.c.set_keys(["A"])
        self.run_frames(3)
        self.assertEqual(self.word(TARGET), 0xA)

    def test_added_while_running_applies_from_a_frame_boundary(self):
        self.c.start()
        try:
            cid = self.c.add_cheat(INCREMENT, "count")
            self.assertGreaterEqual(cid, 1)
        finally:
            self.c.stop_after(0)
            self.c.wait()
        n = self.word(TARGET)
        self.run_frames(4)
        self.assertEqual(self.word(TARGET), n + 4)

    def test_cheat_writes_bypass_watchpoints_and_last_writer(self):
        self.c.track_writers(True)
        self.c.add_watchpoint(TARGET, 4)
        self.c.add_cheat(write32(TARGET, 0x55))
        self.run_frames(3)
        self.assertEqual(self.word(TARGET), 0x55)
        self.assertIsNone(self.c.last_writer(TARGET))


class Patching(CheatTest):
    """A code that patches code changes which functions run; call counts and coverage keep working."""

    def test_call_counts_and_coverage_follow_the_patch(self):
        asm, site, f1, f2 = calls_program()
        self.load_program(asm)
        self.c.track_calls(True)
        self.c.coverage(True)
        self.run_frames(2)
        counts = self.c.take_call_counts(9)
        self.assertGreater(counts.get(f1, 0), 0)
        self.assertNotIn(f2, counts)
        self.c.take_coverage(9)

        cid = self.c.add_cheat(write32(site, bl(site, f2)), "call f2")
        self.run_frames(1)              # the patch lands at this frame's VBlank
        self.c.take_call_counts(9)
        self.c.take_coverage(9)
        self.run_frames(2)
        counts = self.c.take_call_counts(9)
        self.assertNotIn(f1, counts)
        self.assertGreater(counts.get(f2, 0), 0)
        covered = set(self.c.take_coverage(9))
        self.assertIn(f2, covered)
        self.assertNotIn(f1, covered)

        # removing a code does not undo what it wrote: the patch stays until something rewrites it
        self.c.remove_cheat(cid)
        self.run_frames(2)
        counts = self.c.take_call_counts(9)
        self.assertNotIn(f1, counts)
        self.c.write(site, bl(site, f1).to_bytes(4, "little"))
        self.run_frames(2)
        self.assertIn(f1, self.c.take_call_counts(9))


class ListAndGet(CheatTest):
    def test_list_and_get(self):
        long_name = "Always Drop Chest " + "x" * 300
        a = self.c.add_cheat(["020F6D48 0098967F"], "gold")
        b = self.c.add_cheat("94000130 fffb0000\n\n120F5510 0000270F\r\nD2000000 00000000\n", long_name, enabled=False)
        self.assertEqual((a, b), (a, a + 1))
        got = self.c.cheats()
        self.assertEqual([g["id"] for g in got], [a, b])
        self.assertEqual(got[0], {"id": a, "name": "gold", "enabled": True, "words": 2, "code": "020F6D48 0098967F"})
        self.assertEqual(got[1]["name"], long_name)
        self.assertFalse(got[1]["enabled"])
        self.assertEqual(got[1]["code"], "94000130 FFFB0000\n120F5510 0000270F\nD2000000 00000000")
        self.assertEqual(self.c.cheat(b)["words"], 6)

    def test_ids_are_not_reused(self):
        a = self.c.add_cheat(write32(TARGET, 1))
        self.c.remove_cheat(a)
        b = self.c.add_cheat(write32(TARGET, 1))
        self.assertGreater(b, a)
        self.c.clear_cheats()
        self.assertGreater(self.c.add_cheat(write32(TARGET, 1)), b)

    def test_raw_list_truncates_names_and_respects_max(self):
        from ctypes import c_char, c_uint32
        lib = native.lib()
        self.c.add_cheat(write32(TARGET, 1), "n" * 200)
        self.c.add_cheat(write32(TARGET, 2), "second")
        buf = (native.MdsCheat * 4)()
        self.assertEqual(lib.mds_cheat_list(self.c.id, buf, 1), 1)
        self.assertEqual(buf[0].name, b"n" * 115)
        self.assertEqual(lib.mds_cheat_list(self.c.id, buf, 0), 0)
        self.assertEqual(lib.mds_cheat_list(self.c.id, buf, -1), -1)
        self.assertIn("max must not be negative", lib.mds_error().decode())
        words, name = (c_uint32 * 1)(), (c_char * 4)()
        cid = buf[0].cheat
        self.assertEqual(lib.mds_cheat_get(self.c.id, cid, name, 4, words, 1), 2)     # 2 words, 1 copied
        self.assertEqual(name.value, b"nnn")
        self.assertEqual(words[0], TARGET & 0x0FFFFFFF)
        self.assertEqual(lib.mds_cheat_get(self.c.id, cid, None, 0, None, 0), 2)


class Errors(ConsoleTest):
    def assert_fails(self, fn, *parts):
        with self.assertRaises(MelonError) as cm:
            fn()
        for p in parts:
            self.assertIn(p, str(cm.exception))

    def test_bad_code_text(self):
        c = self.c
        self.assert_fails(lambda: c.add_cheat(""), "cheat: the code is empty")
        self.assert_fails(lambda: c.add_cheat("020F6D48 0098967F\n020F6D48"), "cheat: line 2:", "XXXXXXXX YYYYYYYY")
        self.assert_fails(lambda: c.add_cheat("020F6D48 0098967Z"), "cheat: line 1:")
        self.assert_fails(lambda: c.add_cheat("C4000000 00000000"), "C4", "not supported")
        self.assert_fails(lambda: c.add_cheat("DE000000 00000000"), "unknown opcode DE")
        self.assert_fails(lambda: c.add_cheat("E2000000 00000010\n00000000 00000000"), "only 1 lines follow")
        self.assertEqual(c.cheats(), [])

    def test_unknown_ids_and_consoles(self):
        c = self.c
        self.assert_fails(lambda: c.remove_cheat(99), "no cheat 99")
        self.assert_fails(lambda: c.enable_cheat(99), "no cheat 99")
        self.assert_fails(lambda: c.cheat(99), "no cheat 99")
        lib = native.lib()
        self.assertEqual(lib.mds_cheat_add(15, b"", b"02000000 00000000", 1), -1)
        self.assertIn("no console 15", lib.mds_error().decode())
        self.assertEqual(lib.mds_cheat_add(c.id, b"", None, 1), -1)
        self.assertIn("no code text", lib.mds_error().decode())


class Lifetime(CheatTest):
    def test_state_load_clears_the_list(self):
        self.run_frames(2)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "s.mln")
            self.c.save_state(path)
            self.c.add_cheat(INCREMENT, "count")
            self.run_frames(3)
            self.assertEqual(self.word(TARGET), 3)
            self.c.load_state(path)
            self.assertEqual(self.c.cheats(), [])
            self.assertEqual(self.word(TARGET), 0)      # the state's RAM
            self.run_frames(3)
            self.assertEqual(self.word(TARGET), 0)      # and no code runs on it
            self.c.add_cheat(INCREMENT, "count")         # re-added after the load
            self.run_frames(3)
            self.assertEqual(self.word(TARGET), 3)

    def test_rejected_load_keeps_the_list(self):
        self.c.add_cheat(INCREMENT, "count")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "bad.mln")
            with open(path, "wb") as f:
                f.write(b"not a state" * 10)
            with self.assertRaises(MelonError):
                self.c.load_state(path)
        self.assertEqual(len(self.c.cheats()), 1)

    def test_reset_keeps_the_list(self):
        self.c.add_cheat(INCREMENT, "count")
        self.c.reset()
        self.assertEqual([c["name"] for c in self.c.cheats()], ["count"])


class DebugOff(unittest.TestCase):
    """Cheats are not a debugger feature: they run on a console created with debug = 0 too."""

    def test_codes_run_without_the_debugger(self):
        c = native.Console(debug=False)
        self.addCleanup(c.close)
        c.reset()
        c.write(DATA, bytes(0x400))
        main, handler = vblank_irq_program()
        c.write(HANDLER7, handler.assemble(), cpu=7)
        c.write(CODE7, main.assemble(), cpu=7)
        c.set_reg("pc", CODE7, cpu=7)
        c.add_cheat(INCREMENT, "count")
        c.run(6)
        self.assertEqual(int.from_bytes(c.read(TARGET, 4), "little"), 6)


if __name__ == "__main__":
    unittest.main()
