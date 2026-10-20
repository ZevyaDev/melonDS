"""Touch timeline: an ARM7 program samples the touch screen over real SPI transfers at known
scanlines, and the payload's sub-frame points must be what each sample sees."""
import unittest

from . import armasm as A
from .base import CODE7, DATA, ConsoleTest

BUF = DATA + 0x2000
SENTINEL = 0xEEEE
LINE_CYCLES = 2130              # 560190 system cycles per frame / 263 scanlines
LINES = (10, 60, 110, 160)      # where the program samples
SAMPLES_PER_FRAME = len(LINES)

SPICNT = 0x040001C0
HOLD = 0x8000 | 0x0200 | 0x0800     # enabled, touchscreen, chip select held, 4 MHz
LAST = 0x8000 | 0x0200
CMD_X, CMD_Y = 0xD0, 0x90           # start | channel 5 (X) / channel 1 (Y), 12 bit, differential


def at(line, extra=0):
    """Offset in system cycles of the start of a scanline."""
    return line * LINE_CYCLES + extra


def sampler_program():
    """Per frame, at each of LINES: read X and Y through the SPI bus and EXTKEYIN."""
    a = A.Asm(CODE7)
    n = [0]

    def uniq(prefix):
        n[0] += 1
        return f"{prefix}{n[0]}"

    def xfer(byte, hold):
        a.ldr_const(3, HOLD if hold else LAST)
        a.strh(3, 1, 0)
        a.mov(3, byte)
        a.strh(3, 1, 2)
        wait = uniq("busy")
        a.label(wait)
        a.ldrh(3, 1, 0)
        a.word(A._dp_imm(A.AL, A._TST, 1, 3, 0, 0x80))
        a.b(wait, A.NE)

    def sample(cmd):
        xfer(cmd, True)
        xfer(0, True)
        a.ldrh(4, 1, 2)
        a.strh(4, 2, 0)
        a.add(2, 2, 2)
        xfer(0, False)
        a.ldrh(4, 1, 2)
        a.strh(4, 2, 0)
        a.add(2, 2, 2)

    a.ldr_const(0, 0x04000000)
    a.ldr_const(1, SPICNT)
    a.ldr_const(2, BUF)
    a.ldr_const(7, 0x04000130)
    a.label("frame")
    for line in LINES:
        w = uniq("line")
        a.label(w)
        a.ldrh(3, 0, 6)
        a.cmp(3, line)
        a.b(w, A.NE)
        sample(CMD_X)
        sample(CMD_Y)
        a.ldrh(4, 7, 6)
        a.strh(4, 2, 0)
        a.add(2, 2, 2)
    # leave the last line before looping so the next frame's samples are not taken twice
    w = uniq("leave")
    a.label(w)
    a.ldrh(3, 0, 6)
    a.cmp(3, LINES[-1])
    a.b(w, A.EQ)
    a.b("frame")
    return a


class Sample:
    def __init__(self, xh, xl, yh, yl, ext):
        self.x = ((xh << 5) | (xl >> 3)) >> 4       # melonDS stores the screen coordinate << 4
        self.y = ((yh << 5) | (yl >> 3)) >> 4
        self.adc_x = (xh << 5) | (xl >> 3)
        self.adc_y = (yh << 5) | (yl >> 3)
        self.pen_down = not (ext & (1 << 6))

    def __repr__(self):
        return f"Sample(x={self.x}, y={self.y}, down={self.pen_down})"


class TouchTest(ConsoleTest):
    def setUp(self):
        super().setUp()
        self.c.write(BUF, SENTINEL.to_bytes(2, "little") * 512)
        self.c.write(CODE7, sampler_program().assemble(), cpu=7)
        self.c.set_reg("pc", CODE7, cpu=7)

    def line_samples(self):
        """Decode the buffer: per line, [X hi, X lo, Y hi, Y lo, ext] halfwords."""
        raw = self.c.read(BUF, 1024)
        hw = []
        for i in range(0, 1024, 2):
            v = int.from_bytes(raw[i:i + 2], "little")
            if v == SENTINEL:
                break
            hw.append(v)
        # one line = X (2 halfwords) + Y (2 halfwords) + ext (1) = 5
        return [Sample(*hw[i:i + 5]) for i in range(0, len(hw) - len(hw) % 5, 5)]

    def run_and_collect(self, payloads):
        """Submit one payload per frame, run them all and return the per-frame line samples."""
        for p in payloads:
            self.c.submit_input(0, touch=p)
        self.run_frames(len(payloads))
        return self.line_samples()


def tup(s):
    return (s.pen_down, s.x, s.y) if s.pen_down else (False,)


class TouchTimeline(TouchTest):
    def test_sampler_sees_the_frame_start_state_without_a_timeline(self):
        got = self.run_and_collect([[(0, 20, 30, 1)]] * 3)
        self.assertGreaterEqual(len(got), SAMPLES_PER_FRAME * 2)
        for s in got[SAMPLES_PER_FRAME:]:
            self.assertEqual(tup(s), (True, 20, 30))

    def test_single_offset_zero_point_is_the_plain_per_frame_case(self):
        got = self.run_and_collect([[(0, 77, 88, 1)]] * 3)
        self.assertGreaterEqual(len(got), SAMPLES_PER_FRAME * 2)
        for s in got[SAMPLES_PER_FRAME:]:
            self.assertEqual(tup(s), (True, 77, 88))

    def test_points_inside_a_frame_resolve_at_each_read(self):
        # Warm up so the program is in its loop, with the pen down at (20, 30).
        self.run_and_collect([[(0, 20, 30, 1)]] * 2)
        base = len(self.line_samples())
        pts = [(at(35), 100, 50, 1), (at(85), 200, 150, 1), (at(135), 0, 0, 0)]
        got = self.run_and_collect([pts])[base:]
        self.assertEqual(len(got), SAMPLES_PER_FRAME, got)
        self.assertEqual([tup(s) for s in got], [
            (True, 20, 30),         # line 10: before the first point, the previous state
            (True, 100, 50),        # line 60
            (True, 200, 150),       # line 110
            (False,),               # line 160: pen up, EXTKEYIN bit 6 set
        ])
        # Released: the converter returns X = 0, Y = 0xFFF.
        self.assertEqual((got[3].adc_x, got[3].adc_y), (0, 0xFFF))

    def test_pen_up_then_down_flips_extkeyin_mid_frame(self):
        self.run_and_collect([[(0, 5, 6, 0)]] * 2)
        base = len(self.line_samples())
        pts = [(at(35), 9, 8, 1), (at(85), 9, 8, 0), (at(135), 11, 12, 1)]
        got = self.run_and_collect([pts])[base:]
        self.assertEqual([tup(s) for s in got], [(False,), (True, 9, 8), (False,), (True, 11, 12)])

    def test_last_state_carries_into_later_frames(self):
        self.run_and_collect([[(0, 20, 30, 1)]] * 2)
        base = len(self.line_samples())
        pts = [(at(85), 200, 150, 1)]
        got = self.run_and_collect([pts, [], []])[base:]
        self.assertEqual(len(got), 3 * SAMPLES_PER_FRAME, got)
        first, second, third = (got[i * 4:(i + 1) * 4] for i in range(3))
        self.assertEqual([tup(s) for s in first], [(True, 20, 30), (True, 20, 30), (True, 200, 150), (True, 200, 150)])
        for s in second + third:        # no touch points: the previous state, all frame long
            self.assertEqual(tup(s), (True, 200, 150))

    def test_point_before_first_offset_uses_previous_frame_final_state(self):
        self.run_and_collect([[(0, 1, 2, 1), (at(140), 40, 41, 1)]] * 2)
        base = len(self.line_samples())
        got = self.run_and_collect([[(at(85), 90, 91, 1)]])[base:]
        self.assertEqual([tup(s) for s in got], [(True, 40, 41), (True, 40, 41), (True, 90, 91), (True, 90, 91)])

    def test_mds_touch_matches_a_single_offset_zero_payload(self):
        self.run_and_collect([[(0, 20, 30, 1)]] * 2)
        base = len(self.line_samples())
        self.c.touch(66, 77)
        self.run_frames(1)
        via_api = [tup(s) for s in self.line_samples()[base:]]
        self.assertEqual(via_api, [(True, 66, 77)] * SAMPLES_PER_FRAME)
        base = len(self.line_samples())
        self.c.submit_input(0, touch=[(0, 66, 77, 1)])
        self.run_frames(1)
        self.assertEqual([tup(s) for s in self.line_samples()[base:]], via_api)


if __name__ == "__main__":
    unittest.main()
