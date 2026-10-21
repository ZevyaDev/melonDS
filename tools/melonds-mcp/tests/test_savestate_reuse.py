"""A state load fully decides what runs next (API level 7, features ``savestate_derived`` and ``gx_fifo_zeroed``).

A load must leave nothing of the console's previous run: a console that loads a state, a fresh
console that loads it and the console that saved it and ran on all run the same. Two pieces of
CPU state that are not in the state are derived from it on load:

* the CPUs' pending-interrupt flag (from IME, IE and IF): it kept its pre-load value, so a
  pending interrupt was not taken after a load into a console whose flag was clear;
* the ARM9's code fetch timing (from the protection unit and cache settings): it was taken from
  the pre-load protection unit, so after a load the code ran at the speed of whatever the console
  ran before (a reset console fetched cached code at uncached speed) until the next branch.

And GX FIFO entries no longer carry uninitialised padding into states.
"""
import os
import struct
import tempfile
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE7, CODE9, DATA, have_library


def msr_c(imm):
    """msr cpsr_c, #imm"""
    return 0xE321F000 | imm


def mcr15(rd, crn, crm, opc2):
    return 0xEE000F10 | (crn << 16) | (rd << 12) | (opc2 << 5) | crm


def mrc15(rd, crn, crm, opc2):
    return 0xEE100F10 | (crn << 16) | (rd << 12) | (opc2 << 5) | crm


def tst(rn, imm):
    return 0xE3100000 | (rn << 16) | imm


COUNT7 = DATA + 0x100           # IRQ handler count (ARM7)


def cached_block(adds=100_000):
    """ARM9: turn the instruction cache on (PU off: every region's code is cacheable), branch, then
    run a long straight block of adds, so a frame ends inside it, far from the next branch."""
    a = A.Asm(CODE9)
    a.word(mrc15(0, 1, 0, 0))
    a.orr(0, 0, 0x1000)
    a.word(mcr15(0, 1, 0, 0))
    a.mov(1, 0)
    a.b("loop")                             # a branch: from here on code is fetched as cached
    a.label("loop")
    for _ in range(adds):
        a.add(1, 1, 1)
    a.b("loop")
    return a


def pending_irq_then_enable(delay=0x18000):
    """ARM7: raise timer 0's IRQ with IRQs masked (it stays pending in IF), then after a delay that
    outlasts the first frame unmask IRQs; the handler counts at COUNT7 and acknowledges."""
    a = A.Asm(CODE7)
    a.word(msr_c(0xD2))                     # IRQ mode: its stack
    a.ldr_const(13, 0x0380FF00)
    a.word(msr_c(0xDF))                     # SYS mode, IRQ and FIQ masked
    a.ldr_const(0, 0)                       # patched below: handler address
    handler_slot = len(a.pool) - 1
    a.ldr_const(1, 0x03FFFFFC)              # FreeBIOS jumps to [0x03FFFFFC]
    a.str(0, 1)
    a.ldr_const(4, 0x04000000)
    a.mov(0, 0)
    a.str(0, 4, 0x100)                      # timer 0 off
    a.ldr_const(0, COUNT7)
    a.mov(1, 0)
    a.str(1, 0)
    a.mov(0, 8)
    a.str(0, 4, 0x214)                      # IF: clear the timer 0 bit
    a.mov(0, 1)
    a.str(0, 4, 0x208)                      # IME = 1
    a.mov(0, 8)
    a.str(0, 4, 0x210)                      # IE = timer 0
    a.ldr_const(0, 0x00C0FFFF)
    a.str(0, 4, 0x100)                      # timer 0: reload 0xFFFF, IRQ, start
    a.label("wait")
    a.ldr(0, 4, 0x214)
    a.word(tst(0, 8))
    a.b("wait", A.EQ)
    a.mov(0, 0)
    a.str(0, 4, 0x100)                      # timer 0 off: IF stays set, nothing else touches it
    a.ldr_const(6, delay)
    a.label("delay")
    a.subs(6, 6, 1)
    a.b("delay", A.NE)
    a.word(msr_c(0x1F))                     # unmask: the pending IRQ is taken now
    a.label("idle")
    a.add(7, 7, 1)
    a.b("idle")
    a.label("handler")                      # IRQ mode; r0-r3 and r12 are saved by FreeBIOS
    a.ldr_const(0, 0x04000214)
    a.mov(1, 8)
    a.str(1, 0)                             # acknowledge
    a.ldr_const(0, COUNT7)
    a.ldr(1, 0)
    a.add(1, 1, 1)
    a.str(1, 0)
    a.bx(14)
    a.pool[handler_slot] = a.labels["handler"]
    return a


def sections(data):
    out, off = {}, 16
    while off + 16 <= len(data):
        length = struct.unpack_from("<I", data, off + 4)[0]
        if length < 16:
            break
        out[data[off:off + 4].decode("latin1")] = data[off + 16:off + length]
        off += length
    return out


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class Level7Features(unittest.TestCase):
    def test_version_and_features(self):
        self.assertGreaterEqual(native.api_version(), 7)
        for name in ("savestate_derived", "gx_fifo_zeroed"):
            with self.subTest(name):
                self.assertTrue(native.has(name))


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class LoadDecidesTheRun(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def path(self, name):
        return os.path.join(self.tmp.name, name + ".mln")

    def console(self):
        c = native.Console()
        c.reset()
        return c

    def run_frames(self, c, n):
        timeout, states = c.run(n)
        self.assertFalse(timeout)

    def assertSameState(self, a, b):
        with open(a, "rb") as fa, open(b, "rb") as fb:
            sa, sb = sections(fa.read()), sections(fb.read())
        self.assertEqual(sorted(sa), sorted(sb))
        differ = [k for k in sa if sa[k] != sb[k]]
        self.assertEqual(differ, [], "sections that differ")

    def play(self, program, cpu, after, probe):
        """The console that saves S runs on without loading (cont), then loads S (reused: it ran past
        S first); a fresh console loads S. Each runs `after` frames from S and saves. One console at a
        time. Returns probe(console) at S and after each run."""
        out = {}
        a = self.console()
        try:
            a.write(program.base, program.assemble(), cpu=cpu)
            a.set_reg("pc", program.base, cpu=cpu)
            self.run_frames(a, 1)
            a.save_state(self.path("s"))
            out["s"] = probe(a)
            self.run_frames(a, after)
            a.save_state(self.path("cont"))
            out["cont"] = probe(a)
            a.load_state(self.path("s"))
            self.run_frames(a, after)
            a.save_state(self.path("reused"))
            out["reused"] = probe(a)
        finally:
            a.close()
        b = self.console()                                  # fresh: only a reset before the load
        try:
            b.load_state(self.path("s"))
            self.run_frames(b, after)
            b.save_state(self.path("fresh"))
            out["fresh"] = probe(b)
        finally:
            b.close()
        return out

    def test_pending_irq_is_taken_after_a_load(self):
        count = lambda c: int.from_bytes(c.read(COUNT7, 4, cpu=7), "little")
        got = self.play(pending_irq_then_enable(), 7, 8, count)
        self.assertEqual(got, {"s": 0, "cont": 1, "reused": 1, "fresh": 1})
        self.assertSameState(self.path("cont"), self.path("fresh"))
        self.assertSameState(self.path("cont"), self.path("reused"))

    def test_code_timing_comes_from_the_loaded_cache_settings(self):
        r1 = lambda c: c.regs(9)["r1"]
        got = self.play(cached_block(), 9, 1, r1)
        self.assertEqual(got["fresh"], got["cont"])
        self.assertEqual(got["reused"], got["cont"])
        self.assertSameState(self.path("cont"), self.path("fresh"))
        self.assertSameState(self.path("cont"), self.path("reused"))


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class GxFifoPadding(unittest.TestCase):

    def test_fifo_entries_are_zero_padded(self):
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        a = A.Asm(CODE9)
        a.ldr_const(4, 0x04000000)
        a.ldr_const(0, 0x820F)                  # POWCNT1: LCDs, 2D A, 3D render and geometry, 2D B, swap
        a.str(0, 4, 0x304)
        a.ldr_const(5, 0x04000440)              # MTX_MODE
        a.mov(6, 0)
        a.label("again")
        for i in range(48):
            a.mov(0, i & 3)
            a.str(0, 5)                         # MTX_MODE i
            a.str(0, 5, 0x14)                   # MTX_IDENTITY (0x454)
        a.add(6, 6, 1)
        a.b("again")
        c.write(CODE9, a.assemble())
        c.set_reg("pc", CODE9)
        timeout, _ = c.run(2)
        self.assertFalse(timeout)
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "s.mln")
            c.save_state(p)
            with open(p, "rb") as f:
                gp = sections(f.read())["GP3D"]
        entries = gp[12:12 + 256 * 8]
        written = [entries[i:i + 8] for i in range(0, len(entries), 8) if entries[i:i + 8] != bytes(8)]
        self.assertGreater(len(written), 16)    # commands went through the FIFO
        self.assertEqual([e for e in written if e[5:8] != b"\0\0\0"], [])


if __name__ == "__main__":
    unittest.main()
