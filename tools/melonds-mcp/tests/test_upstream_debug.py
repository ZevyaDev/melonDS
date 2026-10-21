"""API level 4 (merge/upstream-debug): thread stacks, a PC change at a break re-running the hooks (and the step
countdown counting from there), pipeline refetch after a write at a break, and Lua memory.execute."""
import ctypes
import unittest
from ctypes import c_uint32

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE9, DATA, ConsoleTest
from .test_api_fixes import adds

MOV_R1_0x55 = 0xE3A01055   # mov r1, #0x55
BX_LR = 0xE12FFF1E


def call_program():
    """mov r1, #0; bl f; back: b back; nop; f (CODE9 + 16): mov r1, #5; bx lr"""
    a = A.Asm(CODE9)
    a.mov(1, 0)
    a.bl("f")
    a.label("back")
    a.b("back")
    a.nop()
    a.label("f")
    a.mov(1, 5)
    a.bx(14)
    return a


F = CODE9 + 16
BACK = CODE9 + 8


class Level4Features(unittest.TestCase):
    def test_version_and_features(self):
        self.assertGreaterEqual(native.api_version(), 4)
        for name in ("thread_stacks", "callstack_unwind", "step_redirect", "pipeline_refetch", "lua_execute",
                     "lua_wifi", "symbols_rom", "mds_threadstacks"):
            with self.subTest(name):
                self.assertTrue(native.has(name))


class ThreadStacks(ConsoleTest):
    def test_negative_counts_fail(self):
        lib, i = native.lib(), self.c.id
        u32 = (c_uint32 * 4)()
        frames = (native.MdsFrame * 4)()
        self.assertEqual(lib.mds_threadstacks(i, 9, u32, u32, -1, frames, 4), -1)
        self.assertIn("max_stacks must not be negative", lib.mds_error().decode())
        self.assertEqual(lib.mds_threadstacks(i, 9, u32, u32, 4, frames, -1), -1)
        self.assertIn("max_frames must not be negative", lib.mds_error().decode())

    def test_needs_the_debugger(self):
        d = native.Console(debug=False)
        d.reset()
        self.addCleanup(d.close)
        with self.assertRaises(MelonError) as cm:
            d.thread_stacks()
        self.assertIn("without the debugger", str(cm.exception))

    def test_a_switched_away_stack_is_listed(self):
        # f is called on one stack, then moves SP far away (another "thread"): the stack with f is parked
        a = A.Asm(CODE9)
        a.word(0xE321F0DF)                  # msr cpsr_c, #0xDF: SYS mode (stacks are not switched in exception modes)
        a.ldr_const(13, DATA + 0x8000)
        a.bl("f")
        a.label("back")
        a.b("back")
        a.label("f")
        a.ldr_const(13, DATA + 0x20000)
        a.label("spin")
        a.b("spin")
        self.c.track_calls(True)
        self.load_program(a)
        self.run_frames(1)
        stacks = self.c.thread_stacks()
        parked = [s for s in stacks if s["sp"] == DATA + 0x8000]
        self.assertEqual(len(parked), 1, stacks)
        self.assertEqual([f["site"] for f in parked[0]["frames"]], [CODE9 + 8])
        self.assertEqual(self.c.callstack(), [])
        # the Lua function sees the same
        out = self.c.lua_exec('for _, s in ipairs(debugger.threadstacks()) do print(s.sp, #s.frames) end')
        self.assertIn(f"{DATA + 0x8000}\t1\n", out)


class StepRedirect(ConsoleTest):
    """Parked at a breakpoint, a new PC is honoured at once: the hooks run again there and mds_step(n) executes n
    instructions from the new PC."""

    def setUp(self):
        super().setUp()
        self.load_program(adds(64))
        self.c.set_reg("r1", 0)
        self.bp = self.c.add_breakpoint(CODE9 + 12)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        self.assertEqual(self.c.take_break().kind, "exec")
        self.assertEqual(self.c.regs()["r1"], 3)

    def test_step_counts_from_the_new_pc(self):
        self.c.remove_point(self.bp)
        self.c.set_reg("pc", CODE9 + 40)
        self.c.step(2)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.pc), ("step", CODE9 + 48))
        self.assertEqual(self.c.regs()["r1"], 5)

    def test_breakpoint_at_the_new_pc_fires(self):
        bp2 = self.c.add_breakpoint(CODE9 + 40)
        self.c.set_reg("pc", CODE9 + 40)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.id, b.pc), ("exec", bp2, CODE9 + 40))
        self.assertEqual(self.c.regs()["r1"], 3)


class PipelineRefetch(ConsoleTest):
    def test_write_at_a_break_changes_the_next_instruction(self):
        self.load_program(adds(64))
        self.c.set_reg("r1", 0)
        bp = self.c.add_breakpoint(CODE9 + 12)
        self.c.run(5)
        self.assertEqual(self.c.take_break().kind, "exec")
        self.c.remove_point(bp)
        # the instruction at the break and the one after it are already in the pipeline
        self.c.write(CODE9 + 12, MOV_R1_0x55.to_bytes(4, "little"))
        self.c.step(1)
        self.c.run(5)
        self.assertEqual(self.c.take_break().kind, "step")
        self.assertEqual(self.c.regs()["r1"], 0x55)


class LuaExecute(ConsoleTest):
    def test_bx_lr_at_a_function_entry_returns_at_once(self):
        self.load_program(call_program())
        self.c.lua_exec(f"event.onmemoryexecute(function() memory.execute({BX_LR}) end, {F})")
        self.run_frames(1)
        r = self.c.regs()
        self.assertEqual(r["r1"], 0)            # f's body never ran
        self.assertEqual(r["pc"] & ~1, BACK)

    def test_result_tells_whether_it_branched(self):
        self.load_program(call_program())
        self.c.lua_exec(f"""
            event.onmemoryexecute(function()
                print(memory.execute(0xE2811001))   -- add r1, r1, #1: no branch
            end, {F})""")
        self.run_frames(1)
        self.assertIn("false", self.c.lua_output())
        self.assertEqual(self.c.regs()["r1"], 5)  # f ran after the injected add (mov r1, #5 overwrites it)

    def test_refused_outside_a_hook(self):
        with self.assertRaises(MelonError) as cm:
            self.c.lua_exec(f"memory.execute({BX_LR})")
        self.assertIn("only inside an exec hook", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
