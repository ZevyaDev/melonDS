"""Command queue behaviour: nothing from another thread touches the NDS mid-frame."""
import threading
import time
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE9, DATA, ConsoleTest

PAIR = DATA + 0x400     # word A at PAIR, word B at PAIR + 4
FREE = DATA + 0x800     # never touched by the programs


def counter_program(base=CODE9, counter=DATA):
    a = A.Asm(base)
    a.ldr_const(0, counter)
    a.label("loop")
    a.ldr(1, 0)
    a.add(1, 1, 1)
    a.str(1, 0)
    a.b("loop")
    return a


def pair_program(base=CODE9, pair=PAIR):
    """Store N to A, wait, store N to B, repeat with N+1. A and B differ for most of the time."""
    a = A.Asm(base)
    a.ldr_const(0, pair)
    a.mov(1, 0)
    a.label("loop")
    a.add(1, 1, 1)
    a.str(1, 0, 0)
    a.spin(2, 200)
    a.str(1, 0, 4)
    a.b("loop")
    return a


class RunningAccess(ConsoleTest):
    def wait_frames(self, n, timeout=20):
        start = self.c.frames
        deadline = time.time() + timeout
        while self.c.frames < start + n:
            self.assertLess(time.time(), deadline, "console stopped making progress")
            time.sleep(0.001)

    def halt(self):
        self.c.stop_after(0)
        timeout, states = self.c.wait()
        self.assertFalse(timeout)
        self.assertEqual(states, [native.IDLE])

    def test_write_while_running_applies_at_frame_boundary(self):
        self.load_program(counter_program())
        self.c.start()
        self.wait_frames(1)
        self.c.write(FREE, (0xCAFEF00D).to_bytes(4, "little"))   # used to fail: "is running"
        frames_after_write = self.c.frames
        self.wait_frames(2)
        self.assertEqual(self.word(FREE), 0xCAFEF00D)
        self.halt()
        self.assertGreater(self.c.frames, frames_after_write)
        self.assertEqual(self.word(FREE), 0xCAFEF00D)

    def test_write_is_visible_to_running_program(self):
        # The program copies [DATA] to [DATA + 8] forever; it never writes DATA itself.
        a = A.Asm(CODE9)
        a.ldr_const(0, DATA)
        a.label("loop")
        a.ldr(1, 0)
        a.str(1, 0, 8)
        a.b("loop")
        self.load_program(a)
        self.c.start()
        self.wait_frames(1)
        self.c.write(DATA, (0x10000000).to_bytes(4, "little"))
        self.wait_frames(2)
        self.halt()
        self.assertEqual(self.word(DATA + 8), 0x10000000)

    def test_reads_while_running_only_see_frame_boundary_states(self):
        # The Lua script records (A, B) at the end of every frame. A read that
        # runs between frames must return one of those pairs; a mid-frame read
        # would mostly return pairs that never existed at a frame end.
        self.load_program(pair_program())
        self.c.lua_run(f"""
            while true do
                print(memory.read_u32_le({PAIR}), memory.read_u32_le({PAIR + 4}))
                emu.frameadvance()
            end""")
        self.c.start()
        observed = []
        for _ in range(60):
            data = self.c.read(PAIR, 8)
            observed.append((int.from_bytes(data[:4], "little"), int.from_bytes(data[4:], "little")))
        self.halt()
        snapshots = set()
        for line in self.c.lua_output().splitlines():
            a, b = line.split()
            snapshots.add((int(a), int(b)))
        self.assertGreater(len(snapshots), 10)
        bad = [p for p in observed if p not in snapshots]
        self.assertEqual(bad, [], "reads returned states that never existed at a frame boundary")

    def test_concurrent_calls_while_running(self):
        self.load_program(counter_program())
        self.c.start()
        errors = []

        def hammer(seed):
            try:
                for i in range(40):
                    self.c.read(DATA, 16)
                    self.c.write(FREE + 16 * seed, bytes([i] * 4))
                    self.c.set_keys(["A"] if i & 1 else [])
            except Exception as e:      # noqa: BLE001
                errors.append(e)

        threads = [threading.Thread(target=hammer, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
            self.assertFalse(t.is_alive(), "caller hung")
        self.assertEqual(errors, [])
        self.halt()


class IdleAndParked(ConsoleTest):
    def test_idle_console_serves_commands(self):
        self.load_program(counter_program())
        self.run_frames(1)
        self.assertEqual(self.c.state, native.IDLE)
        n = self.word(DATA)
        self.assertGreater(n, 0)
        self.assertEqual(self.c.regs()["r0"], DATA)
        self.c.write(FREE, b"\x01\x02\x03\x04")
        self.assertEqual(self.c.read(FREE, 4), b"\x01\x02\x03\x04")
        self.c.set_keys(["A"])
        self.c.touch(10, 20)
        self.c.touch()
        self.c.set_lid(False)
        self.assertEqual(self.word(DATA), n)   # nothing ran in between

    def test_commands_run_while_parked_at_breakpoint(self):
        prog = counter_program()
        self.load_program(prog)
        bp = CODE9 + 4          # the ldr after the literal load
        self.c.add_breakpoint(bp)
        self.c.start()
        timeout, states = self.c.wait(10000)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.PARKED])
        self.assertEqual(self.c.regs()["pc"], bp)
        self.c.write(FREE, b"\xAA\xBB\xCC\xDD")
        self.assertEqual(self.c.read(FREE, 4), b"\xAA\xBB\xCC\xDD")
        self.assertEqual(self.c.regs()["r0"], DATA)
        # still parked, and the console resumes afterwards
        self.assertEqual(self.c.state, native.PARKED)
        self.c.take_break()
        for p in self.c.points():
            self.c.remove_point(p["id"])
        self.c.start(2)
        timeout, states = self.c.wait(10000)
        self.assertEqual(states, [native.IDLE])
        self.assertGreater(self.word(DATA), 0)


if __name__ == "__main__":
    unittest.main()
