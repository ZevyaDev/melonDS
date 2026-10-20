"""input_multi barrier groups, in process: consoles wait at their barrier payload for each other."""
import time
import unittest

from melonds_mcp import native

from .base import ConsoleTest
from .test_input import Akey, Bkey, Down, Left, Recorder, Right, Select, Start, Up, released


class BarrierTest(Recorder, ConsoleTest):
    def setUp(self):
        super().setUp()
        self.c0 = self.c
        self.c1 = native.Console()
        self.c1.reset()
        self.addCleanup(self.c1.close)
        self.install(self.c0)
        self.install(self.c1)

    def wait_until(self, pred, timeout=20, what="condition"):
        deadline = time.time() + timeout
        while not pred():
            self.assertLess(time.time(), deadline, f"timed out waiting for {what}")
            time.sleep(0.002)

    def settle(self, seconds=0.15):
        time.sleep(seconds)

    def finish(self, *consoles):
        for c in consoles:
            timeout, states = c.wait(20000)
            self.assertFalse(timeout)
            self.assertEqual(states, [native.IDLE])

    def multi(self, m0, m1):
        return native.submit_multi([(self.c0, {"buttons": m0}), (self.c1, {"buttons": m1})])


class Barrier(BarrierTest):
    def test_early_console_waits_for_the_late_one(self):
        for m in (Left, Right, Up):
            self.c1.submit_input(m)
        self.assertEqual(self.multi(Akey, Bkey), [1, 4])
        self.c0.start(4)
        self.wait_until(lambda: self.c0.input_waiting(), what="console 0 to wait at the barrier")
        self.settle()
        self.assertEqual(self.c0.frames, 0)
        self.assertEqual(self.c0.state, native.RUNNING)
        self.assertEqual(self.c0.input_depth(), 1)

        self.c1.start(5)
        self.finish(self.c0, self.c1)
        self.assertEqual(self.c0.frames, 4)
        self.assertEqual(self.c1.frames, 5)
        # The barrier payload is the first frame console 0 runs, and console 1's fourth.
        self.assertEqual(self.samples(self.c0), [released(Akey)] * 4)
        self.assertEqual(self.samples(self.c1), [released(m) for m in (Left, Right, Up, Bkey, Bkey)])
        self.assertEqual(self.c0.input_log()[0]["frame"], 0)
        self.assertEqual(self.c1.input_log()[3]["frame"], 3)

    def test_late_console_arrives_and_the_waiting_one_is_released(self):
        # Console 1 runs its three plain payloads first and then waits itself for console 0.
        for m in (Left, Right, Up):
            self.c1.submit_input(m)
        self.multi(Akey, Bkey)
        self.c1.start(5)
        self.wait_until(lambda: self.c1.frames == 3 and self.c1.input_waiting(), what="console 1 at the barrier")
        self.settle()
        self.assertEqual(self.c1.frames, 3)
        self.assertEqual(self.c0.frames, 0)

        self.c0.start(2)
        self.finish(self.c0, self.c1)
        self.assertEqual(self.samples(self.c0), [released(Akey)] * 2)
        self.assertEqual(self.samples(self.c1), [released(m) for m in (Left, Right, Up, Bkey, Bkey)])

    def test_both_run_in_step_after_the_barrier(self):
        # Two groups back to back: neither console gets ahead of the other at either one.
        native.submit_multi([(self.c0, {"buttons": Akey}), (self.c1, {"buttons": Bkey})])
        native.submit_multi([(self.c0, {"buttons": Start}), (self.c1, {"buttons": Select})])
        self.c0.start(2)
        self.c1.start(2)
        self.finish(self.c0, self.c1)
        self.assertEqual(self.samples(self.c0), [released(Akey), released(Start)])
        self.assertEqual(self.samples(self.c1), [released(Bkey), released(Select)])

    def test_waiting_console_still_stops_on_request(self):
        self.multi(Akey, Bkey)
        self.c0.start(10)
        self.wait_until(lambda: self.c0.input_waiting())
        self.c0.stop_after(0)
        self.finish(self.c0)
        self.assertEqual(self.c0.frames, 0)
        self.assertEqual(self.c0.input_depth(), 1)      # the payload is still queued
        # ... and when it runs again it does not wait for a partner that is not coming back.
        self.c0.start(2)
        self.finish(self.c0)
        self.assertEqual(self.samples(self.c0), [released(Akey)] * 2)

    def test_waiting_console_still_interrupts(self):
        self.multi(Akey, Bkey)
        self.c0.start(10)
        self.wait_until(lambda: self.c0.input_waiting())
        self.c0.interrupt()
        self.wait_until(lambda: self.c0.state == native.PARKED, what="console 0 to park")
        self.assertEqual(self.c0.frames, 0)
        self.assertTrue(self.c0.take_break())
        self.c0.start(0)
        self.finish(self.c0)

    def test_one_member_group_does_not_wait(self):
        native.submit_multi([(self.c0, {"buttons": Akey})])
        self.c0.start(2)
        self.finish(self.c0)
        self.assertEqual(self.samples(self.c0), [released(Akey)] * 2)


class BarrierDropOut(BarrierTest):
    """A member that can no longer arrive must never leave the others hanging."""

    def run_waiter(self, frames=3):
        self.multi(Akey, Bkey)
        self.c0.start(frames)
        self.wait_until(lambda: self.c0.input_waiting())
        self.settle(0.05)
        self.assertEqual(self.c0.frames, 0)

    def released(self, frames=3):
        self.finish(self.c0)
        self.assertEqual(self.c0.frames, frames)
        self.assertEqual(self.samples(self.c0), [released(Akey)] * frames)

    def test_destroyed_member_releases(self):
        self.run_waiter()
        self.c1.close()
        self.released()

    def test_member_that_stops_running_releases(self):
        for m in (Left, Right, Up, Down):
            self.c1.submit_input(m)
        self.multi(Akey, Bkey)
        self.c0.start(3)
        self.wait_until(lambda: self.c0.input_waiting())
        self.c1.start(2)                    # runs out of frames before its barrier payload
        self.finish(self.c1)
        self.assertEqual(self.c1.frames, 2)
        self.released()

    def test_cleared_queue_releases(self):
        self.run_waiter()
        self.c1.input_clear()
        self.released()

    def test_reset_releases(self):
        self.run_waiter()
        self.c1.reset()
        self.released()

    def test_dropped_member_does_not_wait_when_it_gets_there(self):
        self.run_waiter()
        self.c1.input_clear()
        self.released()
        # Console 1's payload is gone with the clear; a fresh group works as usual afterwards.
        self.c0.input_clear()
        native.submit_multi([(self.c0, {"buttons": Down}), (self.c1, {"buttons": Up})])
        self.c0.start(1)
        self.c1.start(1)
        self.finish(self.c0, self.c1)

    def test_stopped_member_with_payload_left_does_not_block_later(self):
        for m in (Left, Right):
            self.c1.submit_input(m)
        self.multi(Akey, Bkey)
        self.c1.start(1)                    # stops after one frame; it is dropped from the group
        self.finish(self.c1)
        self.c0.start(2)                    # nobody to wait for
        self.finish(self.c0)
        self.assertEqual(self.samples(self.c0), [released(Akey)] * 2)
        self.c1.start(3)                    # its own barrier payload does not wait either
        self.finish(self.c1)
        self.assertEqual(self.samples(self.c1), [released(m) for m in (Left, Right, Bkey, Bkey)])


class BarrierWireless(BarrierTest):
    """A console blocked in a wireless receive is the one thing that releases a waiter early."""

    def test_partner_waiting_on_wireless_releases_the_waiter(self):
        self.multi(Akey, Bkey)
        self.c0.start(3)
        self.wait_until(lambda: self.c0.input_waiting())
        self.settle(0.05)
        self.assertEqual(self.c0.frames, 0)

        native.debug_recv_pending(self.c1, True)
        self.finish(self.c0)
        native.debug_recv_pending(self.c1, False)
        self.assertEqual(self.c0.frames, 3)
        # It applied its own payload early (on its first frame), so its frame numbering stays in step.
        self.assertEqual(self.samples(self.c0), [released(Akey)] * 3)
        self.assertEqual(self.c0.input_depth(), 0)

        # The partner is still owed its payload, and applies it on its own first frame without waiting.
        self.assertEqual(self.c1.input_depth(), 1)
        self.c1.start(2)
        self.finish(self.c1)
        self.assertEqual(self.samples(self.c1), [released(Bkey)] * 2)

    def test_wireless_receive_that_ended_does_not_release(self):
        native.debug_recv_pending(self.c1, True)
        native.debug_recv_pending(self.c1, False)
        self.multi(Akey, Bkey)
        self.c0.start(1)
        self.wait_until(lambda: self.c0.input_waiting())
        self.settle(0.1)
        self.assertEqual(self.c0.frames, 0)
        self.c0.stop_after(0)
        self.finish(self.c0)

    def test_wireless_receive_of_an_unrelated_console_does_not_release(self):
        c2 = native.Console()
        c2.reset()
        self.addCleanup(c2.close)
        self.multi(Akey, Bkey)
        self.c0.start(1)
        self.wait_until(lambda: self.c0.input_waiting())
        native.debug_recv_pending(c2, True)
        self.settle(0.1)
        self.assertEqual(self.c0.frames, 0)
        native.debug_recv_pending(c2, False)
        self.c0.stop_after(0)
        self.finish(self.c0)


class BarrierAllOrNothing(BarrierTest):
    def test_unknown_console_queues_nothing(self):
        ghost = type("Ghost", (), {"id": 99})()
        with self.assertRaises(native.MelonError) as cm:
            native.submit_multi([(self.c0, {"buttons": Akey}), (ghost, {"buttons": Bkey})])
        self.assertIn("no console", str(cm.exception))
        self.assertEqual(self.c0.input_depth(), 0)

    def test_bad_payload_queues_nothing(self):
        with self.assertRaises(native.MelonError) as cm:
            native.submit_multi([(self.c0, {"buttons": Akey}),
                                 (self.c1, {"buttons": Bkey, "touch": [(10, 1, 1, 1), (10, 2, 2, 1)]})])
        self.assertIn("bad_payload", str(cm.exception))
        self.assertEqual((self.c0.input_depth(), self.c1.input_depth()), (0, 0))

    def test_full_queue_queues_nothing(self):
        for i in range(600):
            self.c1.submit_input(Left, seq=i + 1)
        with self.assertRaises(native.MelonError) as cm:
            self.multi(Akey, Bkey)
        self.assertIn("input_queue_full", str(cm.exception))
        self.assertEqual((self.c0.input_depth(), self.c1.input_depth()), (0, 600))
        self.c1.input_clear()
        # The failed attempt left no group behind: a console with the same ids is not held up.
        self.c0.submit_input(Start)
        self.c0.start(1)
        self.finish(self.c0)

    def test_duplicate_console_is_rejected(self):
        with self.assertRaises(native.MelonError) as cm:
            native.submit_multi([(self.c0, {"buttons": Akey}), (self.c0, {"buttons": Bkey})])
        self.assertIn("bad_payload", str(cm.exception))
        self.assertEqual(self.c0.input_depth(), 0)


if __name__ == "__main__":
    unittest.main()
