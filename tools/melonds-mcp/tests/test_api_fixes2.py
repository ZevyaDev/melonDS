"""The fix/api-quirks-2 fixes (API level 6): interrupts leave no break request armed, the pc of every CPU at a park
is its next instruction, mds_write writes the regions the ARM9 bus refuses bytes for (or fails naming the region),
packets longer than the buffer cannot block mds_packets_take, unknown consoles are errors for mds_frames and
mds_destroy, Lua hooks are tagged and protected in the point list (with a total count), and a GDB restart is an
event the C API sees."""
import ctypes
import time
import unittest
from ctypes import c_int, c_uint8

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest
from .test_api_fixes import adds, dma_program


def spin(base):
    a = A.Asm(base)
    a.label("spin")
    a.b("spin")
    return a


class InterruptLeavesNothingArmed(ConsoleTest):
    """mds_interrupt asks both CPUs to break; only one parks, and the other's request is dropped at the park."""

    def setUp(self):
        super().setUp()
        self.load_program(spin(CODE9))
        self.load_program(spin(CODE7), CODE7, cpu=7)

    def test_resume_after_an_interrupt_runs(self):
        self.c.start()
        time.sleep(0.05)
        self.c.interrupt()
        timeout, states = self.c.wait(10000)
        self.assertEqual((timeout, states), (False, [native.PARKED]))
        self.assertEqual(self.c.take_break().kind, "request")
        # the second CPU's request used to park this run again at once
        self.assertEqual(self.c.run(3), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())

    def test_interrupting_a_stopped_console_does_nothing(self):
        self.c.interrupt()                                  # idle
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())
        bp = self.c.add_breakpoint(CODE9)
        self.assertEqual(self.c.run(2)[1], [native.PARKED])
        self.c.take_break()
        self.c.remove_point(bp)
        self.c.interrupt()                                  # parked
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())

    def test_steps_survive_the_cleanup(self):
        self.c.step(3)
        self.assertEqual(self.c.run(2)[1], [native.PARKED])
        self.assertEqual(self.c.take_break().kind, "step")


class GroupLeavesNothingArmed(unittest.TestCase):
    """mds_run/mds_wait interrupt the other consoles of a group when one parks; their leftover requests are
    dropped too, so the next run of the group runs."""

    def test_group_resume_runs(self):
        a, b = native.Console(), native.Console()
        self.addCleanup(a.close)
        self.addCleanup(b.close)
        for c in (a, b):
            c.reset()
            c.write(CODE9, spin(CODE9).assemble())
            c.set_reg("pc", CODE9)
        bp = a.add_breakpoint(CODE9)
        _, states = native.run_group([a, b], 1000, 10000)
        self.assertEqual(states[0], native.PARKED)
        a.take_break()
        b.take_break()
        a.remove_point(bp)
        self.assertEqual(native.run_group([a, b], 3, 10000), (False, [native.IDLE, native.IDLE]))
        self.assertIsNone(a.take_break())
        self.assertIsNone(b.take_break())


class PcIsTheNextInstruction(ConsoleTest):
    """mds_get_regs index 15 is the address of the instruction each CPU executes next, at every park and for both
    CPUs (it was the debugger's last hooked pc: 0 for a CPU without hooks, one behind for one with hooks)."""

    def setUp(self):
        super().setUp()
        self.load_program(adds(16))
        self.load_program(adds(4096, CODE7), CODE7, cpu=7)
        self.c.set_reg("r1", 0)
        self.c.set_reg("r1", 0, cpu=7)

    def park_at_arm9(self, addr):
        self.c.add_breakpoint(addr)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.cpu, b.pc), ("exec", 9, addr))

    def check_arm7(self):
        r = self.c.regs(7)
        self.assertEqual(r["pc"], r["r15_raw"] - 4)
        self.assertEqual(r["pc"], CODE7 + 4 * r["r1"])      # r1 adds have run: the next one is r1 + 1
        return r["pc"]

    def test_breaking_cpu(self):
        self.park_at_arm9(CODE9 + 12)
        r = self.c.regs()
        self.assertEqual((r["pc"], r["r1"]), (CODE9 + 12, 3))

    def test_cpu_without_hooks(self):
        self.park_at_arm9(CODE9 + 12)
        self.assertNotEqual(self.check_arm7(), 0)

    def test_cpu_with_hooks_is_not_one_behind(self):
        self.c.add_breakpoint(0x02FF0000, cpu=7)           # never reached: arms the ARM7's hooks
        self.c.trace(64, cpu=7)
        self.park_at_arm9(CODE9 + 12)
        self.c.take_trace()
        pc = self.check_arm7()
        # the pc reported is the next instruction the ARM7 runs
        self.c.step(1, cpu=7)
        self.assertEqual(self.c.run(5)[1], [native.PARKED])
        trace = [t for t in self.c.take_trace() if t[1] == 7]
        self.assertEqual(trace[0][2], pc)

    def test_after_a_pc_change(self):
        self.park_at_arm9(CODE9 + 12)
        self.c.set_reg("pc", CODE9 + 40)
        self.assertEqual(self.c.regs()["pc"], CODE9 + 40)

    def test_idle(self):
        self.assertEqual(self.c.regs()["pc"], CODE9)
        self.assertEqual(self.c.regs(7)["pc"], CODE7)

    def test_dma_break(self):
        src, dst = DATA + 0x500, DATA + 0x600
        prog = dma_program(src, dst)
        done = prog.labels["done"]
        self.load_program(prog)
        self.c.add_watchpoint(dst, 16, write=True)
        _, states = self.c.run(3)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual(b.pc, native.DMA_TAG)               # the event: the DMA tag, no instruction
        r = self.c.regs()
        # the CPUs are between two instructions: the ARM9's next is the one after the store that started the DMA
        self.assertEqual(r["pc"], r["r15_raw"] - 4)
        self.assertEqual(r["pc"], done)
        self.check_arm7()


class FrameBoundaryRequestPc(ConsoleTest):
    def test_request_at_a_frame_boundary_reports_an_instruction(self):
        self.load_program(spin(CODE9))
        self.c.input_mode("wait")
        self.c.start(100)
        deadline = time.time() + 10
        while not self.c.input_waiting():
            self.assertLess(time.time(), deadline)
            time.sleep(0.01)
        self.c.interrupt()
        _, states = self.c.wait(10000)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.cpu), ("request", 9))
        r = self.c.regs()
        self.assertEqual(b.pc, r["pc"])
        self.assertEqual(b.pc, CODE9)                        # the spin loop, not R15 (CODE9 + 8)
        self.c.input_mode("repeat")
        self.c.stop_after(0)
        self.c.wait(10000)


if __name__ == "__main__":
    unittest.main()
