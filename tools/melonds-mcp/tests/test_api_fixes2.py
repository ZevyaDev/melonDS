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


if __name__ == "__main__":
    unittest.main()
