"""Input over the socket: one payload per frame, frame events, modes, stall timeout, limits."""
import struct
import time

from melonds_mcp import native
from melonds_mcp.client import T

from .server_base import ServerTest
from .test_input import Akey, Bkey, Left, Recorder, Right, Start, Up, released


class ServerInputTests(Recorder, ServerTest):
    def setUp(self):
        super().setUp()
        self.install(self.c)

    def test_wait_mode_applies_one_payload_per_frame_with_events(self):
        cl = self.connect(empty_queue="wait")
        masks = [Akey, Bkey, Start, Up, Akey | Bkey]
        seqs = []
        for i, m in enumerate(masks):
            self.assertEqual(cl.input(buttons=m), i + 1)
            seqs.append(cl.last_seq)
        cl.subscribe(1, [])
        cl.live_start()
        events = [cl.next_event("frame", timeout=10) for _ in masks]
        self.assertTrue(all(events))
        self.assertEqual([e.frame for e in events], [1, 2, 3, 4, 5])
        self.assertEqual([e.applied_seq for e in events], seqs)
        self.assertTrue(all(e.input_applied for e in events))
        self.assertEqual([e.queue_depth for e in events], [4, 3, 2, 1, 0])
        self.assertEqual([e.waiting_for_input for e in events], [False] * 4 + [True])
        self.assertTrue(all(e.console == self.c.id for e in events))
        self.wait_until(lambda: self.c.input_waiting(), what="console to wait")
        self.assertIsNone(cl.next_event("frame", timeout=0.2))
        cl.live_stop()
        self.wait_until(lambda: self.c.state == native.IDLE)
        self.assertEqual(self.samples(), [released(m) for m in masks])

    def test_no_frame_events_without_subscription(self):
        cl = self.connect()
        cl.input(buttons=["A"])
        cl.run(5)
        self.assertEqual(cl.take_events("frame"), [])

    def test_repeat_mode_event_flags(self):
        cl = self.connect()
        cl.subscribe(1, [])
        cl.input(buttons=["A"])
        cl.run(3)
        evs = cl.take_events("frame")
        self.assertEqual([e.input_applied for e in evs], [True, False, False])
        self.assertEqual([e.waiting_for_input for e in evs], [False] * 3)

    def test_stall_timeout_releases_client_buttons_after_15_frames(self):
        cl = self.connect()
        cl.input(buttons=Akey)
        cl.run(30)
        self.assertEqual(self.samples(), [released(Akey)] * 16 + [0x3FF] * 14)

    def test_stall_timeout_option(self):
        cl = self.connect(stall_timeout=2)
        cl.input(buttons=Akey)
        cl.run(6)
        self.assertEqual(self.samples(), [released(Akey)] * 3 + [0x3FF] * 3)

    def test_set_option_stall_zero_never_releases(self):
        cl = self.connect()
        cl.set_stall_timeout(0)
        cl.input(buttons=Akey)
        cl.run(30)
        self.assertEqual(self.samples(), [released(Akey)] * 30)

    def test_neutral_mode(self):
        cl = self.connect(empty_queue="neutral")
        cl.input(buttons=Left)
        cl.input(buttons=Right)
        cl.run(4)
        self.assertEqual(self.samples(), [released(Left), released(Right), 0x3FF, 0x3FF])

    def test_set_option_empty_queue_applies_to_header_console_only(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.set_empty_queue("wait", console=c2.id)
        cl.live_start(0, [c2.id, self.c.id])
        self.wait_until(lambda: c2.input_waiting(), what="console 2 to wait")
        self.wait_until(lambda: self.c.frames > 3, what="console 1 to keep running")
        self.assertFalse(self.c.input_waiting())
        cl.live_stop()

    def test_set_option_empty_queue_all_consoles(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.set_empty_queue("wait", console=0xFFFF)
        cl.live_start()
        self.wait_until(lambda: c2.input_waiting() and self.c.input_waiting(), what="both to wait")
        cl.live_stop()

    def test_switching_away_from_wait_releases_console(self):
        cl = self.connect(empty_queue="wait")
        cl.live_start()
        self.wait_until(lambda: self.c.input_waiting())
        cl.set_empty_queue("repeat")
        self.wait_until(lambda: self.c.frames > 5, what="console to run again")
        cl.live_stop()

    def test_queue_full(self):
        cl = self.connect()
        for i in range(600):
            cl.input(buttons=0)
        self.expect_error("input_queue_full", cl.input, buttons=0)
        self.assertEqual(self.c.input_depth(), 600)

    def test_bad_payloads(self):
        cl = self.connect()
        bad_buttons = struct.pack("<IHBBBBHH", 1, 0x1000, 0, 0, 0, 0, 0, 0)
        self.expect_error("bad_payload", cl.request, T.input, bad_buttons[4:])
        self.expect_error("bad_payload", cl.request, T.input, b"\x01\x00")
        self.expect_error("bad_payload", cl.input, touch=[(5, 1, 1, 1), (5, 2, 2, 1)])
        self.expect_error("bad_payload", cl.input, touch=[(0, 300, 1, 1)])
        self.assertEqual(self.c.input_depth(), 0)

    def test_touch_and_mic_payloads_are_accepted(self):
        cl = self.connect()
        self.assertEqual(cl.input(touch=(10, 20)), 1)
        self.assertEqual(cl.input(touch=[(0, 1, 2, 1), (1000, 3, 4, 0)]), 2)
        self.assertEqual(cl.input(mic=[1, -2, 3]), 3)
        self.assertEqual(cl.input(mic_level=1000, lid=True), 4)

    def test_input_to_specific_console(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.input(buttons=Akey, console=c2.id)
        self.assertEqual(cl.last_console, c2.id)
        self.assertEqual(c2.input_depth(), 1)
        self.assertEqual(self.c.input_depth(), 0)

    def test_input_log_records_client_seq(self):
        cl = self.connect()
        cl.input(buttons=Akey)
        seq = cl.last_seq
        cl.run(2)
        log = self.c.input_log()
        self.assertEqual(log[0]["seq"], seq)


class ServerInputMultiTests(Recorder, ServerTest):
    def setUp(self):
        super().setUp()
        self.c1 = self.new_console()
        self.install(self.c)
        self.install(self.c1)

    def test_barrier_over_the_socket(self):
        cl = self.connect()
        for m in (Left, Right, Up):
            cl.input(buttons=m, console=self.c1.id)
        depths = cl.input_multi([(self.c.id, {"buttons": Akey}), (self.c1.id, {"buttons": Bkey})])
        self.assertEqual(depths, [1, 4])
        seqs = cl.last_seqs
        self.assertEqual(len(set(seqs)), 2)
        cl.subscribe(1, [], console=self.c.id)
        cl.subscribe(1, [], console=self.c1.id)
        cl.run(5)
        self.assertEqual(self.samples(self.c), [released(Akey)] * 5)
        self.assertEqual(self.samples(self.c1), [released(m) for m in (Left, Right, Up, Bkey, Bkey)])
        events = cl.take_events("frame")
        e0 = [e for e in events if e.console == self.c.id]
        self.assertEqual(e0[0].applied_seq, seqs[0])
        self.assertEqual(self.c1.input_log()[3]["seq"], seqs[1])

    def test_waiting_member_holds_the_other_back(self):
        cl = self.connect()
        cl.input_multi([(self.c.id, {"buttons": Akey}), (self.c1.id, {"buttons": Bkey})])
        pending = cl.run(2, consoles=[self.c.id], wait=False)
        self.wait_until(lambda: self.c.input_waiting(), what="console 0 to wait at the barrier")
        time.sleep(0.1)
        self.assertEqual(self.c.frames, 0)
        cl.run(2, consoles=[self.c1.id])
        pending.result()
        self.assertEqual(self.c.frames, 2)

    def test_destroyed_member_releases_the_other(self):
        cl = self.connect()
        cl.input_multi([(self.c.id, {"buttons": Akey}), (self.c1.id, {"buttons": Bkey})])
        cl.live_start(consoles=[self.c.id])
        self.wait_until(lambda: self.c.input_waiting())
        self.c1.close()
        self.wait_until(lambda: self.c.frames > 3, what="console 0 to run")
        cl.live_stop()

    def test_unknown_console_is_no_console_and_nothing_is_queued(self):
        cl = self.connect()
        self.expect_error("no_console", cl.input_multi, [(self.c.id, {"buttons": Akey}), (77, {"buttons": Bkey})])
        self.assertEqual(self.c.input_depth(), 0)

    def test_bad_entry_applies_none(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.input_multi, [
            (self.c.id, {"buttons": Akey}),
            (self.c1.id, {"buttons": Bkey, "touch": [(5, 1, 1, 1), (5, 2, 2, 1)]})])
        self.assertEqual((self.c.input_depth(), self.c1.input_depth()), (0, 0))

    def test_full_queue_on_one_console_applies_none(self):
        cl = self.connect()
        for i in range(600):
            self.c1.submit_input(Left, seq=i + 1)
        self.expect_error("input_queue_full", cl.input_multi,
                          [(self.c.id, {"buttons": Akey}), (self.c1.id, {"buttons": Bkey})])
        self.assertEqual((self.c.input_depth(), self.c1.input_depth()), (0, 600))

    def test_empty_and_duplicate_lists_are_rejected(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.input_multi, [])
        self.expect_error("bad_payload", cl.input_multi,
                          [(self.c.id, {"buttons": Akey}), (self.c.id, {"buttons": Bkey})])
        self.assertEqual(self.c.input_depth(), 0)
