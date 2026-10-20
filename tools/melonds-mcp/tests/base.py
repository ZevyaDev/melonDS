"""Shared helpers for the integration tests (unittest, no pytest).

Findings about synthetic programs (consoles boot FreeBIOS + generated firmware, no cart):

* ``native.Console()`` with no arguments creates a console, but run() does nothing until the
  first ``reset()`` (that is what starts the NDS). ``reset()`` with no cart works (Instance::Reset
  skips direct boot when no cart is inserted; it used to crash). No ROM is needed.
* Code written with Console.write to main RAM (0x02000000+) is executed correctly by both CPUs
  (the interpreter is used, so there is no JIT cache to invalidate). Use ``load_program``.
* Setting PC with Console.set_reg("pc", addr) works for ARM state (the helper passes an even
  address; mds_set_reg adds the current T bit, so a CPU in ARM state stays in ARM state).
* Without a cart, FreeBIOS leaves both CPUs spinning in BIOS code (not halted), so jumping
  away from there is enough: nothing needs to be un-halted.
* ARM9 and ARM7 each run a program from main RAM at once; give them different code and data
  regions. Keep test data away from 0x02000000-0x020FFFFF where FreeBIOS/firmware use RAM.
"""
import os
import sys
import unittest
from pathlib import Path

from melonds_mcp import native

CODE9 = 0x02100000
CODE7 = 0x02180000
DATA = 0x02200000


def have_library():
    try:
        native.lib()
        return True
    except Exception:
        return False


class ConsoleTest(unittest.TestCase):
    """Creates one console per test; frees it afterwards."""

    def setUp(self):
        self.console = native.Console()
        self.console.reset()  # starts the NDS; run() does nothing before the first reset
        self.addCleanup(self.console.close)
        self.c = self.console

    def load_program(self, code, addr=CODE9, cpu=9):
        """Write machine code (bytes or Asm) at addr and point the CPU's PC at it."""
        if hasattr(code, "assemble"):
            code = code.assemble()
        self.c.write(addr, code, cpu=cpu)
        self.c.set_reg("pc", addr, cpu=cpu)

    def word(self, addr, cpu=9):
        return int.from_bytes(self.c.read(addr, 4, cpu=cpu), "little")

    def run_frames(self, n):
        timeout, states = self.c.run(n)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.IDLE])
