"""Microphone input: payload samples and noise levels reach the TSC mic channel through the per-console FIFO."""
import unittest

from melonds_mcp import native

from . import armasm as A
from .base import CODE7, DATA, ConsoleTest

LOG = DATA + 0x4000
LOG_BYTES = 6 * 263 * 14
END = 0xEEEE
SPICNT = 0x040001C0
LINES = 263
SILENT = 0x800              # 12-bit mid scale: sample 0
HELD = 0x800


def level_of(sample):
    """The 12-bit value the TSC returns for an s16 mic sample."""
    return ((sample & 0xFFFF) ^ 0x8000) >> 4


def sampler(entries):
    """ARM7: once per scanline read the mic channel through the touchscreen SPI device and store
    (vcount, high byte, low byte) as three halfwords, ``entries`` times, then spin."""
    a = A.Asm(CODE7)
    a.ldr_const(0, 0x04000000)
    a.ldr_const(4, SPICNT)
    a.ldr_const(1, LOG)
    a.ldr_const(2, 0x1FF)
    a.ldr_const(5, entries)
    a.ldr_const(8, 0x8A00)      # enable, hold chip select, touchscreen
    a.ldr_const(9, 0x8200)      # enable, touchscreen, release after this byte
    n = [0]

    def xfer(val):
        n[0] += 1
        a.mov(7, val)
        a.strh(7, 4, 2)
        a.label(f"w{n[0]}")
        a.ldrh(7, 4, 0)
        a.word(A._dp_imm(A.AL, A._AND, 1, 7, 7, 0x80))      # ands r7, r7, #0x80: the busy bit
        a.b(f"w{n[0]}", A.NE)

    a.label("loop")
    a.ldrh(3, 0, 6)
    a.cmp_reg(3, 2)
    a.b("loop", A.EQ)
    a.mov_reg(2, 3)
    a.strh(8, 4, 0)
    xfer(0xE0)                  # start, channel 6 (mic), 12 bit
    xfer(0)
    a.ldrh(10, 4, 2)
    a.strh(9, 4, 0)
    xfer(0)
    a.ldrh(11, 4, 2)
    a.strh(3, 1, 0)
    a.strh(10, 1, 2)
    a.strh(11, 1, 4)
    a.add(1, 1, 6)
    a.subs(5, 5, 1)
    a.b("loop", A.NE)
    a.label("done")
    a.b("done")
    return a


def parse_log(raw):
    """-> list of (frame, value). Frames are counted from VCOUNT wrapping."""
    out = []
    frame = 0
    prev = None
    for i in range(0, len(raw) - 5, 6):
        vc, hi, lo = (int.from_bytes(raw[i + k:i + k + 2], "little") for k in (0, 2, 4))
        if vc == END:
            break
        if prev is not None and vc < prev:
            frame += 1
        prev = vc
        out.append((frame, ((hi << 8) | lo) >> 3))
    return out


class MicTest(ConsoleTest):
    def start_sampler(self, c, entries):
        c.write(LOG, END.to_bytes(2, "little") * (LOG_BYTES // 2))
        c.write(CODE7, sampler(entries).assemble(), cpu=7)
        c.set_reg("pc", CODE7, cpu=7)

    def log(self, c):
        return parse_log(c.read(LOG, LOG_BYTES))

    def run_scenario(self, payloads, frames, entries=LINES * 12):
        """Fresh console; one payload kwargs dict per frame; returns the parsed sample log."""
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        self.start_sampler(c, entries)
        for p in payloads:
            c.submit_input(**p)
        timeout, states = c.run(frames)
        self.assertFalse(timeout)
        self.assertEqual(states, [native.IDLE])
        return self.log(c)


class MicSamples(MicTest):
    def test_idle_mic_reads_mid_scale(self):
        log = self.run_scenario([{}] * 4, 4)
        self.assertGreater(len(log), LINES * 3)
        self.assertEqual({v for _, v in log}, {SILENT})

    def test_constant_samples_appear_within_four_frames(self):
        pay = [{"mic": [0x4000] * 796}] * 10
        log = self.run_scenario(pay, 10)
        first = next((f for f, v in log if v == level_of(0x4000)), None)
        self.assertIsNotNone(first, "samples never reached the game")
        self.assertGreaterEqual(first, 1)       # the payload of frame 0 arrives while the mic is still off
        self.assertLessEqual(first, 4)
        after = [v for f, v in log if f > first]
        self.assertEqual(set(after), {level_of(0x4000)})

    def test_ramp_is_identical_on_every_run(self):
        ramp = [(i * 80) - 32000 for i in range(796)]
        pay = [{"mic": ramp}] * 8
        a = self.run_scenario(pay, 8)
        b = self.run_scenario(pay, 8)
        self.assertEqual(a, b)
        self.assertGreater(len({v for _, v in a}), 50)

    def test_ramp_values_come_from_the_payload(self):
        ramp = [(i * 80) - 32000 for i in range(796)]
        log = self.run_scenario([{"mic": ramp}] * 8, 8)
        allowed = {level_of(s) for s in ramp} | {SILENT}
        self.assertTrue({v for _, v in log} <= allowed)
        self.assertGreater(len({v for _, v in log}), 50)

    def test_short_payload_holds_the_last_sample(self):
        # frame 0 only starts the mic (its payload would be dropped), so send the samples in frame 1
        log = self.run_scenario([{}, {"mic": [0x2000] * 10}] + [{}] * 6, 8)
        vals = [v for _, v in log]
        self.assertIn(level_of(0x2000), vals)
        # nothing more is queued: the last sample is held, not replaced by silence
        tail = vals[-LINES:]
        self.assertEqual(set(tail), {level_of(0x2000)})


class MicLevel(MicTest):
    def test_level_zero_is_silent(self):
        log = self.run_scenario([{"mic_level": 0}] * 8, 8)
        self.assertEqual({v for _, v in log}, {SILENT})

    def test_level_makes_bounded_noise(self):
        level = 0x2000
        log = self.run_scenario([{"mic_level": level}] * 8, 8)
        vals = [v for f, v in log if f >= 5]
        self.assertGreater(len(set(vals)), 20)
        bound = level // 2 // 16 + 1
        self.assertLessEqual(max(vals), SILENT + bound)
        self.assertGreaterEqual(min(vals), SILENT - bound)
        self.assertGreater(max(vals) - min(vals), bound // 2)

    def test_level_noise_repeats_exactly(self):
        pay = [{"mic_level": 50000}] * 8
        self.assertEqual(self.run_scenario(pay, 8), self.run_scenario(pay, 8))


class MicLifecycle(MicTest):
    # The FIFO flush itself is covered by the unit tests (MicFifoClearAndActive): with one payload
    # per frame the core's own buffer pulls the FIFO empty almost at once, so little is left to flush.
    def test_samples_sent_while_the_mic_is_off_never_arrive(self):
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        # phase 1: sample for about two frames while distinct payloads arrive
        self.start_sampler(c, LINES * 2)
        for k in range(10):
            c.submit_input(mic=[0x1000 + 0x1000 * k] * 796)
        c.run(2)
        first = {v for _, v in self.log(c)}
        self.assertGreater(len(first - {SILENT}), 0, "phase 1 never saw any samples")
        # the game stops sampling; the mic turns off after two frames and drops what is queued
        c.run(7)
        # phase 2: sample again with no new samples queued
        c.input_clear()
        self.start_sampler(c, LINES * 4)
        c.submit_input()
        c.run(4)
        again = {v for _, v in self.log(c)}
        self.assertEqual(len(again), 1, f"stale samples leaked: {sorted(again)}")


if __name__ == "__main__":
    unittest.main()


class MicAcrossLoad(MicTest):
    """The same state and inputs give the same mic samples whether or not a state load happened: the noise is
    seeded from the emulated frame (which states carry), and the samples queued in the FIFO and in the core's
    buffer are saved with the state."""

    def scenario_with_load(self, first, detour, rest):
        import os
        import tempfile
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        self.start_sampler(c, LINES * 12)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "mic.mln")
            for p in first:
                c.submit_input(**p)
            c.run(len(first))
            c.save_state(path)
            for p in detour:
                c.submit_input(**p)
            c.run(len(detour))
            c.load_state(path)
            for p in rest:
                c.submit_input(**p)
            c.run(len(rest))
        return self.log(c)

    def test_level_noise_is_the_same_after_a_load(self):
        pay = [{"mic_level": 40000}] * 10
        straight = self.run_scenario(pay, 10)
        loaded = self.scenario_with_load(pay[:5], [{"mic_level": 1000}] * 3, pay[5:])
        self.assertGreater(len({v for _, v in straight}), 20)
        self.assertEqual(loaded, straight)

    def test_samples_queued_at_the_save_survive_the_load(self):
        ramp = [(i * 80) - 32000 for i in range(796)]
        pay = [{"mic": ramp}] * 10
        straight = self.run_scenario(pay, 10)
        loaded = self.scenario_with_load(pay[:5], [{"mic": [0x7000] * 796}] * 2, pay[5:])
        self.assertEqual(loaded, straight)
