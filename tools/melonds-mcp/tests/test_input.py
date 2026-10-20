"""Input queue: one payload per frame, empty-queue modes, per-console queues, setters and Lua."""
import time
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest

BUF9 = DATA + 0x1000
BUF7 = DATA + 0x2000
KEYINPUT = 0x04000130       # ARM9 (and ARM7) 16-bit, active low, 10 keys
EXTKEYIN = 0x04000136       # ARM7 only: bit 6 pen (0 = down), bit 7 lid (1 = closed)
SENTINEL = 0xEEEE

Akey, Bkey, Select, Start, Right, Left, Up, Down, R, L = (1 << i for i in range(10))


def released(mask):
    return ~mask & 0x3FF


def recorder(base, io, buf):
    """Once per frame (when VCOUNT reaches 100) store the 16-bit register at ``io`` to buf[i++]."""
    a = A.Asm(base)
    a.ldr_const(0, 0x04000000)
    a.ldr_const(4, io)
    a.ldr_const(1, buf)
    a.label("loop")
    a.label("w1")
    a.ldrh(2, 0, 6)
    a.cmp(2, 100)
    a.b("w1", A.NE)
    a.ldrh(3, 4, 0)
    a.strh(3, 1, 0)
    a.add(1, 1, 2)
    a.label("w2")
    a.ldrh(2, 0, 6)
    a.cmp(2, 100)
    a.b("w2", A.EQ)
    a.b("loop")
    return a


class Recorder:
    def install(self, c, keys=True, ext=False):
        c.write(BUF9, SENTINEL.to_bytes(2, "little") * 128)
        c.write(BUF7, SENTINEL.to_bytes(2, "little") * 128)
        c.write(CODE9, recorder(CODE9, KEYINPUT, BUF9).assemble())
        c.set_reg("pc", CODE9)
        c.write(CODE7, recorder(CODE7, EXTKEYIN, BUF7).assemble(), cpu=7)
        c.set_reg("pc", CODE7, cpu=7)

    def samples(self, c=None, buf=BUF9):
        c = c or self.c
        raw = c.read(buf, 256)
        out = []
        for i in range(0, 256, 2):
            v = int.from_bytes(raw[i:i + 2], "little")
            if v == SENTINEL:
                break
            out.append(v)
        return out


class InputTest(Recorder, ConsoleTest):
    def setUp(self):
        super().setUp()
        self.install(self.c)

    def wait_until(self, pred, timeout=20, what="condition"):
        deadline = time.time() + timeout
        while not pred():
            self.assertLess(time.time(), deadline, f"timed out waiting for {what}")
            time.sleep(0.002)


class QueueBasics(InputTest):
    def test_payloads_apply_one_per_frame_in_order(self):
        masks = [Akey, Bkey, Start, Up, Akey | Bkey]
        for m in masks:
            self.c.submit_input(m)
        self.assertEqual(self.c.input_depth(), 5)
        self.run_frames(5)
        self.assertEqual(self.c.input_depth(), 0)
        self.assertEqual(self.samples(), [released(m) for m in masks])

    def test_key_names_accepted(self):
        self.c.submit_input(["A", "Down"])
        self.run_frames(1)
        self.assertEqual(self.samples(), [released(Akey | Down)])

    def test_repeat_holds_last_state_when_queue_empties(self):
        self.c.submit_input(Left)
        self.c.submit_input(Right)
        self.run_frames(5)
        self.assertEqual(self.samples(), [released(Left)] + [released(Right)] * 4)

    def test_neutral_releases_when_queue_empties(self):
        self.c.input_mode("neutral")
        self.c.submit_input(Left)
        self.c.submit_input(Right)
        self.run_frames(4)
        self.assertEqual(self.samples(), [released(Left), released(Right), 0x3FF, 0x3FF])

    def test_client_payload_stalls_in_repeat_mode(self):
        self.c.input_mode("repeat", stall_frames=2)
        self.c.submit_input(Akey, client=True)
        self.run_frames(6)
        self.assertEqual(self.samples(), [released(Akey)] * 3 + [0x3FF] * 3)

    def test_local_payload_never_stalls(self):
        self.c.input_mode("repeat", stall_frames=2)
        self.c.submit_input(Akey)
        self.run_frames(6)
        self.assertEqual(self.samples(), [released(Akey)] * 6)

    def test_stall_zero_disables_timeout(self):
        self.c.input_mode("repeat", stall_frames=0)
        self.c.submit_input(Akey, client=True)
        self.run_frames(40)
        self.assertEqual(self.samples(), [released(Akey)] * 40)

    def test_full_queue_rejects_without_partial_apply(self):
        for i in range(600):
            self.c.submit_input(Akey, seq=i + 1)
        with self.assertRaises(native.MelonError) as cm:
            self.c.submit_input(Bkey, seq=601)
        self.assertIn("input_queue_full", str(cm.exception))
        self.assertEqual(self.c.input_depth(), 600)
        self.c.input_clear()
        self.assertEqual(self.c.input_depth(), 0)

    def test_bad_payload_is_rejected(self):
        pts = [(i * 100, 1, 1, 1) for i in range(17)]
        with self.assertRaises(native.MelonError) as cm:
            self.c.submit_input(0, touch=pts)
        self.assertIn("bad_payload", str(cm.exception))
        with self.assertRaises(native.MelonError):
            self.c.submit_input(0, touch=[(10, 1, 1, 1), (10, 2, 2, 1)])
        with self.assertRaises(native.MelonError):
            self.c.submit_input(0, mic=[0] * 797)
        self.assertEqual(self.c.input_depth(), 0)

    def test_log_records_frame_and_seq(self):
        self.c.submit_input(Akey, seq=41, client=True)
        self.c.submit_input(Bkey, seq=42, client=True)
        self.c.submit_input(Start, seq=43)      # local: logged with seq 0
        self.run_frames(4)
        log = self.c.input_log()
        self.assertEqual([e["frame"] for e in log], [0, 1, 2])
        self.assertEqual([e["seq"] for e in log], [41, 42, 0])
        self.assertEqual([e["buttons"] for e in log], [Akey, Bkey, Start])
        self.assertEqual([e["frame"] for e in self.c.input_log(from_frame=1, max=1)], [1])

    def test_reset_clears_queue_and_log(self):
        self.c.submit_input(Akey)
        self.run_frames(1)
        self.c.submit_input(Bkey)
        self.c.submit_input(Bkey)
        self.assertEqual(len(self.c.input_log()), 1)
        self.c.reset()
        self.assertEqual(self.c.input_depth(), 0)
        self.assertEqual(self.c.input_log(), [])


class WaitMode(InputTest):
    def setUp(self):
        super().setUp()
        self.c.input_mode("wait")

    def test_waits_for_each_frame_of_input(self):
        for m in (Akey, Bkey, Start):
            self.c.submit_input(m)
        self.c.start(100)
        self.wait_until(lambda: self.c.input_waiting(), what="the console to wait")
        self.assertEqual(self.c.state, native.RUNNING)
        time.sleep(0.05)
        self.assertEqual(self.c.frames, 3)
        self.c.submit_input(Up)
        self.c.submit_input(Down)
        self.wait_until(lambda: self.c.frames == 5 and self.c.input_waiting(), what="two more frames")
        time.sleep(0.05)
        self.assertEqual(self.c.frames, 5)
        self.assertEqual(self.c.state, native.RUNNING)
        self.assertEqual(self.samples(), [released(m) for m in (Akey, Bkey, Start, Up, Down)])
        self.c.stop_after(0)
        timeout, states = self.c.wait(10000)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.IDLE])
        self.assertFalse(self.c.input_waiting())

    def test_leaving_wait_mode_releases(self):
        self.c.start(10)
        self.wait_until(lambda: self.c.input_waiting())
        self.c.input_mode("repeat")
        timeout, states = self.c.wait(10000)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.IDLE])
        self.assertEqual(self.c.frames, 10)

    def test_interrupt_parks_at_the_frame_boundary(self):
        self.c.submit_input(Akey)
        self.c.start(100)
        self.wait_until(lambda: self.c.input_waiting())
        self.c.interrupt()
        timeout, states = self.c.wait(10000)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.PARKED])
        self.assertEqual(self.c.frames, 1)
        brk = self.c.take_break()
        self.assertIsNotNone(brk)
        self.assertEqual(brk.kind, "request")
        self.assertFalse(self.c.input_waiting())
        # resuming goes back to waiting for input
        self.c.start(100)
        self.wait_until(lambda: self.c.input_waiting())
        self.assertEqual(self.c.state, native.RUNNING)
        self.c.submit_input(Bkey)
        self.wait_until(lambda: self.c.frames == 2 and self.c.input_waiting())
        self.c.stop_after(0)
        self.c.wait(10000)

    def test_commands_run_while_waiting(self):
        self.c.start(100)
        self.wait_until(lambda: self.c.input_waiting())
        self.c.write(DATA + 0x3000, b"\x01\x02\x03\x04")
        self.assertEqual(self.c.read(DATA + 0x3000, 4), b"\x01\x02\x03\x04")
        self.assertTrue(self.c.input_waiting())
        self.c.stop_after(0)
        self.c.wait(10000)


class TwoConsoles(Recorder, unittest.TestCase):
    def test_each_console_sees_only_its_own_input(self):
        a, b = native.Console(), native.Console()
        self.addCleanup(a.close)
        self.addCleanup(b.close)
        helper = self
        for c in (a, b):
            c.reset()
            helper.install(c)
        for m in (Akey, Bkey, Start):
            a.submit_input(m)
        for m in (Up, Down, Left):
            b.submit_input(m)
        timeout, states = native.run_group([a, b], 3, 60000)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.IDLE, native.IDLE])
        self.assertEqual(helper.samples(a), [released(m) for m in (Akey, Bkey, Start)])
        self.assertEqual(helper.samples(b), [released(m) for m in (Up, Down, Left)])

    def test_wait_mode_is_per_console(self):
        a, b = native.Console(), native.Console()
        self.addCleanup(a.close)
        self.addCleanup(b.close)
        for c in (a, b):
            c.reset()
        a.input_mode("wait")
        a.submit_input(Akey)
        a.start(50)
        b.start(7)
        timeout, states = native.wait_group([b], 20000)
        self.assertFalse(timeout)
        deadline = time.time() + 20
        while not a.input_waiting():
            self.assertLess(time.time(), deadline)
            time.sleep(0.002)
        self.assertEqual(a.frames, 1)
        self.assertEqual(b.frames, 7)
        self.assertFalse(b.input_waiting())
        a.stop_after(0)
        native.wait_group([a], 10000)


class Setters(InputTest):
    def test_setters_apply_at_the_next_frame_not_immediately(self):
        self.run_frames(1)
        self.c.set_keys(["A"])
        # Queued, not applied: the keys land at the next frame start.
        self.assertEqual(self.c.input_depth(), 1)
        self.assertEqual(len(self.c.input_log()), 0)
        self.run_frames(1)
        self.assertEqual(self.samples()[-1], released(Akey))

    def test_keys_and_touch_before_a_frame_land_together(self):
        self.run_frames(1)
        self.c.set_keys(["A"])
        self.c.touch(10, 10)
        self.assertEqual(self.c.input_depth(), 1)       # amended in place
        self.run_frames(1)
        self.assertEqual(self.samples()[-1], released(Akey))
        ext = self.samples(buf=BUF7)
        self.assertEqual(ext[-1] & (1 << 6), 0)          # pen down is active low

    def test_setters_keep_the_other_state(self):
        self.c.touch(10, 10)
        self.run_frames(1)
        self.c.set_keys(["B"])                           # does not release the pen
        self.run_frames(1)
        self.assertEqual(self.samples()[-1], released(Bkey))
        self.assertEqual(self.samples(buf=BUF7)[-1] & (1 << 6), 0)
        self.c.touch()
        self.run_frames(1)
        self.assertEqual(self.samples(buf=BUF7)[-1] & (1 << 6), 1 << 6)

    def test_setter_goes_behind_queued_payloads(self):
        self.c.submit_input(Up)
        self.c.submit_input(Down)
        self.c.set_keys(["Start"])
        self.assertEqual(self.c.input_depth(), 3)
        self.run_frames(3)
        self.assertEqual(self.samples(), [released(Up), released(Down), released(Start)])

    def test_lid(self):
        self.c.submit_input(0, lid=True)
        self.c.submit_input(0, lid=True)
        self.c.submit_input(0, lid=False)
        self.run_frames(3)
        ext = self.samples(buf=BUF7)
        self.assertEqual([(v >> 7) & 1 for v in ext], [1, 1, 0])
        self.c.set_lid(True)
        self.run_frames(1)
        self.assertEqual((self.samples(buf=BUF7)[-1] >> 7) & 1, 1)


class LuaInput(InputTest):
    def test_joypad_set_lands_in_the_queue_after_queued_payloads(self):
        self.c.submit_input(Bkey)
        self.c.submit_input(Bkey)
        self.c.lua_run("while true do joypad.set({A=true}); emu.frameadvance() end")
        self.assertEqual(self.c.input_depth(), 3)
        self.run_frames(4)
        # The script's payload starts from the newest state (B is still held) and adds A.
        self.assertEqual(self.samples(), [released(Bkey)] * 2 + [released(Akey | Bkey)] * 2)
        self.c.lua_stop()

    def test_joypad_get_reports_the_pending_state(self):
        self.c.lua_run("joypad.set({Up=true}); local t = joypad.get(); print(t.Up, t.A)")
        self.assertEqual(self.c.lua_output().strip(), "true\tfalse")
        self.run_frames(1)
        self.assertEqual(self.samples(), [released(Up)])

    def test_nds_tap(self):
        self.c.lua_run("while true do input.NDSTapDown(20, 30); emu.frameadvance() end")
        self.run_frames(2)
        self.assertEqual(self.samples(buf=BUF7)[-1] & (1 << 6), 0)
        self.c.lua_stop()


if __name__ == "__main__":
    unittest.main()
