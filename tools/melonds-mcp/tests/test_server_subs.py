"""Subscriptions, frame events and back-pressure."""
import struct

from melonds_mcp import native

from .base import CODE9, DATA
from .server_base import ServerTest
from .test_server import COUNTER, counter_program


class SubscriptionTests(ServerTest):
    def test_one_event_per_frame_with_range_bytes(self):
        self.c.write(DATA + 0x600, bytes(range(8)))
        self.c.write(DATA + 0x700, b"abcd")
        cl = self.connect()
        cl.subscribe(7, [(9, DATA + 0x600, 8), (9, DATA + 0x700, 4)])
        cl.run(5)
        evs = cl.take_events("frame")
        self.assertEqual([e.frame for e in evs], [1, 2, 3, 4, 5])
        for e in evs:
            self.assertEqual(e.blocks, {7: [bytes(range(8)), b"abcd"]})
            self.assertEqual(e.console, self.c.id)

    def test_values_are_from_the_event_frame(self):
        self.c.write(CODE9, counter_program())
        self.c.set_reg("pc", CODE9)
        cl = self.connect()
        cl.subscribe(1, [(9, COUNTER, 4)])
        cl.run(6)
        values = [struct.unpack("<I", e.blocks[1][0])[0] for e in cl.take_events("frame")]
        self.assertEqual(len(values), 6)
        self.assertEqual(values, sorted(values))
        self.assertGreater(values[-1], values[0])

    def test_every_n_frames(self):
        cl = self.connect()
        cl.subscribe(2, [(9, DATA, 4)], every=3)
        cl.run(10)
        self.assertEqual([e.frame for e in cl.take_events("frame")], [3, 6, 9])

    def test_unsubscribe_stops_events(self):
        cl = self.connect()
        cl.subscribe(2, [(9, DATA, 4)])
        cl.run(2)
        self.assertEqual(len(cl.take_events("frame")), 2)
        cl.unsubscribe(2)
        cl.run(2)
        self.assertEqual(cl.take_events("frame"), [])

    def test_unsubscribe_unknown_id(self):
        cl = self.connect()
        self.expect_error("failed", cl.unsubscribe, 99)

    def test_two_subscriptions_share_an_event(self):
        cl = self.connect()
        cl.subscribe(1, [(9, DATA, 2)])
        cl.subscribe(2, [(9, DATA + 4, 3)])
        cl.run(1)
        (e,) = cl.take_events("frame")
        self.assertEqual({k: [len(r) for r in v] for k, v in e.blocks.items()}, {1: [2], 2: [3]})

    def test_zero_range_subscription_gives_plain_events(self):
        cl = self.connect()
        cl.subscribe(5)
        cl.run(3)
        evs = cl.take_events("frame")
        self.assertEqual([e.frame for e in evs], [1, 2, 3])
        self.assertEqual(evs[0].blocks, {5: []})

    def test_resubscribing_an_id_replaces_it(self):
        cl = self.connect()
        cl.subscribe(1, [(9, DATA, 2)])
        cl.subscribe(1, [(9, DATA, 6)])
        cl.run(1)
        (e,) = cl.take_events("frame")
        self.assertEqual(e.blocks, {1: [bytes(6)]})

    def test_only_subscribed_console_sends_events(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.subscribe(1, [], console=self.c.id)
        cl.run(3, [self.c.id, c2.id])
        evs = cl.take_events("frame")
        self.assertEqual({e.console for e in evs}, {self.c.id})

    def test_too_many_ranges(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.subscribe, 1, [(9, DATA, 1)] * 65)

    def test_range_on_arm7(self):
        self.c.write(DATA + 0x900, b"\x01\x02", cpu=7)
        cl = self.connect()
        cl.subscribe(1, [(7, DATA + 0x900, 2)])
        cl.run(1)
        (e,) = cl.take_events("frame")
        self.assertEqual(e.blocks[1], [b"\x01\x02"])


class BackPressureTests(ServerTest):
    def test_slow_client_gets_dropped_events_and_emulator_keeps_running(self):
        cl = self.connect()
        cl.subscribe(1, [(9, 0x02000000, 0x200000)])
        cl.live_start()
        self.wait_until(lambda: self.c.frames >= 90, timeout=60, what="emulator to keep running")
        cl.live_stop()
        self.wait_until(lambda: self.c.state == native.IDLE, timeout=30)
        total = self.c.frames
        # the live_stop reply (a reply, never dropped) arrives behind the queued events
        evs = cl.take_events("frame")
        for _ in range(100):
            if evs and evs[-1].frame == total:
                break
            cl.pump(0.1)
            evs += cl.take_events("frame")
        dropped = sum(e.dropped for e in evs)
        self.assertGreater(dropped, 0)
        self.assertEqual(len(evs) + dropped, total)
        # a reply still gets through afterwards
        self.assertEqual(len(cl.read(DATA, 4)), 4)
