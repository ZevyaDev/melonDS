"""CLI: input sequences (expansion and applied), input log, subscribe, savestates, screenshot."""
import json
import struct
import unittest
import zlib
from pathlib import Path

from melonds_mcp.client import Client

from .base import DATA
from .cli_base import CliConsole, CliTest
from .test_input import BUF9, CODE9, Akey, Down, Left, Recorder, released


class SequenceExpansion(unittest.TestCase):
    def test_keys_waits_and_default_length(self):
        from melonds_mcp.cli.sequence import expand
        frames = expand("A*4 w20 Down*30")
        self.assertEqual(len(frames), 54)
        self.assertEqual([f.buttons for f in frames[:4]], [Akey] * 4)
        self.assertEqual({f.buttons for f in frames[4:24]}, {0})
        self.assertEqual({f.buttons for f in frames[24:]}, {Down})
        self.assertTrue(all(f.touch is None for f in frames))
        self.assertEqual([f.buttons for f in expand("A+Left, w1")], [Akey | Left] * 4 + [0])
        self.assertEqual(len(expand("a")), 4)            # default 4 frames, key names case-insensitive

    def test_touch_tokens(self):
        from melonds_mcp.cli.sequence import expand
        frames = expand("touch:10,20*3 w1 A+touch:5:6*2")
        self.assertEqual([f.touch for f in frames[:3]], [[(0, 10, 20, 1)]] * 3)
        self.assertEqual(frames[3].touch, [(0, 10, 20, 0)])      # the wait releases the pen once
        self.assertEqual((frames[4].buttons, frames[4].touch), (Akey, [(0, 5, 6, 1)]))
        # a sequence that ends touching gets one trailing release frame
        end = expand("touch:1,2*2")
        self.assertEqual(len(end), 3)
        self.assertEqual(end[-1].touch, [(0, 1, 2, 0)])

    def test_bad_input(self):
        from melonds_mcp.cli.sequence import expand
        for bad in ("Bogus*3", "A*x", "touch:300,2*1", "touch:1*2"):
            with self.assertRaises(ValueError, msg=bad):
                expand(bad)


class InputTests(Recorder, CliTest):
    def setUp(self):
        super().setUp()
        self.install(CliConsole(self))

    def samples(self, count):
        raw = self.read_bytes(BUF9, count * 2)
        return [int.from_bytes(raw[i:i + 2], "little") for i in range(0, len(raw), 2)]

    def test_sequence_applied_in_wait_mode(self):
        r = self.cli("input", "A*3 w2 Down*2")
        self.assertEqual(r["payloads"], 7)
        self.assertEqual(r["frames_run"], 7)
        self.assertEqual(self.samples(7), [released(Akey)] * 3 + [0x3FF] * 2 + [released(Down)] * 2)

    def test_neutral_mode_releases_after_the_sequence(self):
        self.cli("input", "Left*2", "--mode", "neutral", "--frames", 4)
        self.assertEqual(self.samples(4), [released(Left)] * 2 + [0x3FF] * 2)

    def test_repeat_mode_holds_the_last_payload(self):
        self.cli("input", "Left*2", "--mode", "repeat", "--frames", 4)
        self.assertEqual(self.samples(4), [released(Left)] * 4)

    def test_no_run_only_queues(self):
        r = self.cli("input", "A*3", "--no-run")
        self.assertEqual(r["frames_run"], 0)
        self.assertEqual(r["queue_depth"], 3)

    def test_long_sequence_is_chunked(self):
        r = self.cli("input", "w700")
        self.assertEqual(r["frames_run"], 700)

    def test_input_log_and_out_file(self):
        self.cli("input", "A*2 w1")
        log = self.cli("input-log")
        self.assertEqual([(e["frame"], e["buttons"]) for e in log["entries"]], [(0, Akey), (1, Akey), (2, 0)])
        out = self.tmpfile("log.json")
        r = self.cli("input-log", "--from", 1, "--out", out)
        self.assertEqual(r["count"], 2)
        saved = json.loads(Path(out).read_text())
        self.assertEqual([e["frame"] for e in saved["entries"]], [1, 2])


class SubscribeTests(CliTest):
    def test_subscribe_collects_frame_events(self):
        self.cli("write", hex(DATA), "--hex", "11223344")
        r = self.run_cli("subscribe", "--range", f"{DATA:#x}:4", "--frames", 3)
        self.assertEqual(r.code, 0, r.stdout + r.stderr)
        self.assertEqual(len(r.lines), 3)
        self.assertEqual([e["frame"] for e in r.lines], [1, 2, 3])
        self.assertEqual(r.lines[0]["data"], ["11223344"])
        self.assertEqual(r.lines[0]["console"], 0)
        # subscription is gone after the command: no more events are buffered for the next client
        self.assertEqual(self.cli("run", 2)["frames"], 2)

    def test_every_n(self):
        r = self.run_cli("subscribe", "--frames", 2, "--every", 3)
        self.assertEqual([e["frame"] for e in r.lines], [3, 6])


class StateTests(CliTest):
    def test_state_save_load_round_trip(self):
        path = self.tmpfile("a.state")
        self.write_hex(DATA, b"\xaa\xaa\xaa\xaa")
        self.assertEqual(self.cli("state", "save", path)["ok"], True)
        self.assertTrue(Path(path).stat().st_size > 1000)
        self.write_hex(DATA, b"\xbb\xbb\xbb\xbb")
        self.cli("state", "load", path)
        self.assertEqual(self.read_bytes(DATA, 4), b"\xaa" * 4)

    def test_state_load_missing_file(self):
        self.assertTrue(self.cli_error("state", "load", self.tmpfile("nope.state"))["error"])


class ScreenshotTests(CliTest):
    def test_png_is_valid_and_matches_raw_screenshot(self):
        self.cli("run", 3)
        out = self.tmpfile("shot.png")
        r = self.cli("screenshot", out)
        self.assertEqual((r["width"], r["height"], r["file"]), (256, 384, out))
        data = Path(out).read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        chunks = {}
        pos = 8
        while pos < len(data):
            n, kind = struct.unpack(">I4s", data[pos:pos + 8])
            body = data[pos + 8:pos + 8 + n]
            self.assertEqual(struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0], zlib.crc32(kind + body))
            chunks.setdefault(kind, []).append(body)
            pos += 12 + n
        w, h, depth, ctype, _, _, _ = struct.unpack(">IIBBBBB", chunks[b"IHDR"][0])
        self.assertEqual((w, h, depth), (256, 384, 8))
        self.assertIn(ctype, (2, 6))
        self.assertIn(b"IEND", chunks)
        raw = zlib.decompress(b"".join(chunks[b"IDAT"]))
        bpp = 3 if ctype == 2 else 4
        stride = 1 + w * bpp
        self.assertEqual(len(raw), stride * h)
        with Client(self.port) as c:
            c.hello()
            _, pixels = c.screenshot()
        for y in (0, 5, 191, 192, 383):
            row = raw[y * stride:(y + 1) * stride]
            self.assertEqual(row[0], 0)
            for x in (0, 7, 100, 255):
                b, g, rr, _a = pixels[(y * 256 + x) * 4:(y * 256 + x) * 4 + 4]
                self.assertEqual(tuple(row[1 + x * bpp:1 + x * bpp + 3]), (rr, g, b))

    def test_screen_crop(self):
        out = self.tmpfile("top.png")
        r = self.cli("screenshot", out, "--screen", "top")
        self.assertEqual((r["width"], r["height"]), (256, 192))
