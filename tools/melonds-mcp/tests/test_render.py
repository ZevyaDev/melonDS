"""Render control (API level 7, feature/render-control): mds_set_render skips 2D / 3D drawing, mds_set_threaded_3d
draws 3D on its own thread, mds_render_stats reports both. Neither may change emulated state (RAM, VRAM, the whole
savestate) or, once drawing is back on long enough, the screen.

The synthetic scene (no cart) is driven by an ARM9 program: at VCount 250 of every frame it copies a mailbox in main
RAM to the display registers (I/O cannot be written by the debugger). Python fills the mailbox between frames: the
2D backdrops and the 3D clear colour change every frame, BG0 shows the 3D layer, and display capture (DISPCAPCNT)
copies the 2D or 3D output to VRAM bank A when asked. Writing the capture request at VCount 250, after the 3D frame
was started at 215, is the hard case: the 3D frame the capture needs was skipped and is drawn late.

With MELONDS_ROM and MDS_TEST_STATE (a savestate of that ROM, .mln or .mln.gz) set, a real game runs too."""
import gzip
import hashlib
import os
import shutil
import tempfile
import unittest
from pathlib import Path

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE9, DATA

MAILBOX = DATA
# (register, store size) the program copies from the mailbox, in mailbox order (one word each)
REGS = [(0x04000304, "h"),      # POWCNT1
        (0x04000000, "w"),      # DISPCNT A
        (0x04001000, "w"),      # DISPCNT B
        (0x04000240, "b"),      # VRAMCNT_A
        (0x05000000, "h"),      # backdrop A
        (0x05000400, "h"),      # backdrop B
        (0x04000350, "w"),      # CLEAR_COLOR
        (0x04000064, "w")]      # DISPCAPCNT
VRAM_A = 0x06800000
FRAMES = 60


def program():
    a = A.Asm(CODE9)
    a.label("top")
    a.ldr_const(2, 0x04000006)          # VCOUNT
    a.label("wait")
    a.ldrh(3, 2)
    a.cmp(3, 250)
    a.b("wait", A.NE)
    a.ldr_const(0, MAILBOX)
    for i, (addr, size) in enumerate(REGS):
        a.ldr(1, 0, 4 * i)
        a.ldr_const(4, addr)
        {"h": a.strh, "w": a.str, "b": a.strb}[size](1, 4)
    a.label("leave")
    a.ldrh(3, 2)
    a.cmp(3, 250)
    a.b("leave", A.EQ)
    a.b("top")
    return a.assemble()


def mailbox(i, capture=None):
    """Frame i's register values; capture 0 = capture the 2D output, 1 = the 3D output, None = no capture."""
    return [0x820F,                                     # LCD, 2D A, 3D render + geometry, 2D B; A on top
            0x00010108,                                 # display on, BG0 on, BG0 = 3D
            0x00010000,
            0x80,                                       # bank A as LCDC (the capture's destination)
            (i * 7) & 0x7FFF,
            (i * 13 + 5) & 0x7FFF,
            ((i * 31 + 3) & 0x7FFF) | ((31 if i % 3 else 0) << 16),    # opaque, or transparent every 3rd frame
            0 if capture is None else (1 << 31) | (3 << 20) | (capture << 24)]


def sha(data):
    return hashlib.sha1(bytes(data)).hexdigest()


def no_capture(i):
    return None


def capture_every_frame(i):
    return i % 2


def capture_now_and_then(i):
    return (i // 10) % 2 if i % 10 == 0 else None


BASE = []


def base_state():
    """One console's state right after a reset, shared by every run: a new console's clock and generated firmware
    user settings come from the host's clock, and savestates keep both."""
    if not BASE:
        d = tempfile.mkdtemp(prefix="render-scene-")
        c = native.Console()
        try:
            c.reset()
            c.save_state(str(Path(d) / "base.mln"))
        finally:
            c.close()
        BASE.append((d, str(Path(d) / "base.mln")))
    return BASE[0][1]


def tearDownModule():
    for d, _ in BASE:
        shutil.rmtree(d, ignore_errors=True)


def run_scene(skip=0, threaded=False, capture=no_capture, draw_from=None, state=None):
    """Run the scene for FRAMES frames: (screen hash per frame, VRAM A hash, render stats). With draw_from, skipping
    ends before that frame. With state, the whole state is saved there at the end."""
    base = base_state()
    c = native.Console()
    try:
        c.reset()
        c.load_state(base)
        c.set_threaded_3d(threaded)
        c.set_render_flags(skip)
        c.write(CODE9, program())
        c.set_reg("pc", CODE9)
        screens = []
        for i in range(FRAMES):
            if i == draw_from:
                c.set_render_flags(0)
            c.write(MAILBOX, b"".join(v.to_bytes(4, "little") for v in mailbox(i, capture(i))))
            timeout, states = c.run(1)
            assert not timeout and states == [native.IDLE]
            screens.append(sha(c.screen()))
        vram = sha(c.read(VRAM_A, 0x20000))
        stats = c.render_stats()
        if state:
            c.save_state(state)
        return screens, vram, stats
    finally:
        c.close()


class Level7Features(unittest.TestCase):
    def test_version_and_features(self):
        self.assertGreaterEqual(native.api_version(), 7)
        for name in ("render_skip", "threaded_3d", "render_stats", "mds_set_render", "mds_set_threaded_3d",
                     "mds_render_stats"):
            with self.subTest(name):
                self.assertTrue(native.has(name))


class Calls(unittest.TestCase):
    def setUp(self):
        self.c = native.Console()
        self.addCleanup(self.c.close)
        self.c.reset()

    def test_flags_apply_at_the_next_frame(self):
        self.c.set_render_flags(native.RENDER_SKIP_ALL)
        st = self.c.render_stats()
        self.assertEqual((st["skip_wanted"], st["skip"]), (3, 0))      # asked for, not in force before a frame
        self.c.run(2)
        st = self.c.render_stats()
        self.assertEqual(st["skip"], 3)
        self.assertEqual(st["lines_2d_skipped"], 2 * 192)
        self.assertEqual(st["frames_3d_skipped"], 2)
        self.c.set_render(skip_2d=False, skip_3d=False)
        self.c.run(1)
        st = self.c.render_stats()
        self.assertEqual((st["skip_wanted"], st["skip"], st["lines_2d_skipped"]), (0, 0, 2 * 192))

    def test_threaded_applies_at_once_on_a_stopped_console(self):
        self.c.set_threaded_3d(True)
        st = self.c.render_stats()
        self.assertEqual((st["threaded_wanted"], st["threaded"]), (1, 1))
        self.c.run(3)
        self.c.set_threaded_3d(False)
        self.assertEqual(self.c.render_stats()["threaded"], 0)

    def test_errors(self):
        with self.assertRaisesRegex(MelonError, "unknown render flags"):
            self.c.set_render_flags(4)
        for call in (lambda: native.lib().mds_set_render(9999, 0), lambda: native.lib().mds_set_threaded_3d(9999, 1)):
            self.assertEqual(call(), -1)
        out = (native.c_uint64 * 4)()
        self.assertEqual(native.lib().mds_render_stats(self.c.id, out, 4), native.lib().mds_render_stats(self.c.id, out, 0))
        self.assertEqual(native.lib().mds_render_stats(self.c.id, out, -1), -1)


class Scene(unittest.TestCase):
    """Every mode gives the reference's VRAM (what display capture wrote) and, after drawing again, its screens."""

    @classmethod
    def setUpClass(cls):
        cls.ref = {cap: run_scene(capture=cap) for cap in (no_capture, capture_every_frame, capture_now_and_then)}

    def test_reference_scene_moves(self):
        screens, _, _ = self.ref[no_capture]
        self.assertEqual(len(set(screens)), FRAMES)     # every frame looks different

    def test_threaded_3d_draws_the_same_frames(self):
        for cap, (screens, vram, _) in self.ref.items():
            with self.subTest(cap.__name__):
                got = run_scene(threaded=True, capture=cap)
                self.assertEqual(got[0], screens)
                self.assertEqual(got[1], vram)

    def test_frames_needed_after_skipping(self):
        """After skipping ends, 2D is right from the first frame drawn and 3D from the second (a frame shows the 3D
        drawn during the previous frame's VBlank): RENDER_FRAMES_2D, RENDER_FRAMES_3D."""
        screens = self.ref[no_capture][0]
        at = 40
        for skip, needed in ((native.RENDER_SKIP_2D, native.RENDER_FRAMES_2D),
                             (native.RENDER_SKIP_3D, native.RENDER_FRAMES_3D),
                             (native.RENDER_SKIP_ALL, native.RENDER_FRAMES_3D)):
            for threaded in (False, True):
                with self.subTest(skip=skip, threaded=threaded):
                    got = run_scene(skip, threaded, draw_from=at)[0]
                    self.assertNotEqual(got[at - 1], screens[at - 1])   # skipping showed a stale screen
                    self.assertEqual(got[at + needed - 1:], screens[at + needed - 1:])
                    if needed > 1:
                        self.assertNotEqual(got[at], screens[at])       # one frame is not enough for 3D

    def test_capture_keeps_vram(self):
        """Display capture writes VRAM: skipping still draws what it needs, whether the capture was requested
        before the 3D frame started (every frame) or after it (now and then: drawn late)."""
        for cap in (capture_every_frame, capture_now_and_then):
            vram = self.ref[cap][1]
            for skip in (native.RENDER_SKIP_2D, native.RENDER_SKIP_3D, native.RENDER_SKIP_ALL):
                for threaded in (False, True):
                    with self.subTest(cap=cap.__name__, skip=skip, threaded=threaded):
                        _, got, st = run_scene(skip, threaded, capture=cap)
                        self.assertEqual(got, vram)
                        if skip & native.RENDER_SKIP_2D:
                            self.assertGreater(st["capture_lines"], 0)
                        if skip & native.RENDER_SKIP_3D:
                            self.assertGreater(st["frames_3d_for_capture"], 0)
                            self.assertGreater(st["late_3d_frames"], 0)
                            self.assertGreater(st["frames_3d_skipped"], 0)

    def test_savestates_are_identical(self):
        """The whole emulated state (RAM, VRAM, registers, 3D engine) does not depend on drawing or threads."""
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, (skip, threaded) in enumerate(((0, False), (native.RENDER_SKIP_ALL, False),
                                                   (native.RENDER_SKIP_ALL, True), (0, True))):
                p = Path(d) / f"s{i}.mln"
                run_scene(skip, threaded, capture=capture_now_and_then, state=str(p))
                paths.append(p.read_bytes())
            self.assertTrue(all(p == paths[0] for p in paths[1:]))


@unittest.skipUnless(os.environ.get("MELONDS_ROM") and os.environ.get("MDS_TEST_STATE"),
                     "set MELONDS_ROM and MDS_TEST_STATE (a savestate of that ROM) to run a real game")
class Game(unittest.TestCase):
    """A few hundred frames of a real game from a savestate, with some input: the same savestate bytes and main RAM
    in every mode, and the reference screen once the last RENDER_FRAMES_3D frames are drawn."""
    FRAMES = 300
    KEYS = [0, 1, 0, 0x10, 0, 0x40, 0, 0x20, 0x80, 0, 2, 0]

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix="render-test-"))
        src = os.environ["MDS_TEST_STATE"]
        cls.state = cls.dir / "start.mln"
        if src.endswith(".gz"):
            with gzip.open(src, "rb") as f, open(cls.state, "wb") as g:
                shutil.copyfileobj(f, g)
        else:
            shutil.copyfile(src, cls.state)
        cls.ref = cls.play(0, False)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    @classmethod
    def play(cls, skip, threaded, draw_last=None):
        c = native.Console()
        try:
            c.load_rom(os.environ["MELONDS_ROM"], str(cls.dir / "blank.sav"))
            c.load_state(str(cls.state))
            c.set_threaded_3d(threaded)
            c.set_render_flags(skip)
            for i in range(cls.FRAMES):
                if draw_last and i == cls.FRAMES - draw_last:
                    c.set_render_flags(0)
                c.set_keys(cls.KEYS[(i // 20) % len(cls.KEYS)])
                c.run(1)
            out = cls.dir / "end.mln"
            c.save_state(str(out))
            return sha(out.read_bytes()), sha(c.read(0x02000000, 0x400000)), sha(c.screen())
        finally:
            c.close()

    def test_modes(self):
        for skip, threaded, draw_last in ((0, True, None), (native.RENDER_SKIP_ALL, False, native.RENDER_FRAMES_3D),
                                          (native.RENDER_SKIP_ALL, True, native.RENDER_FRAMES_3D),
                                          (native.RENDER_SKIP_2D, False, native.RENDER_FRAMES_2D)):
            with self.subTest(skip=skip, threaded=threaded):
                state, ram, screen = self.play(skip, threaded, draw_last)
                self.assertEqual((state, ram, screen), self.ref)


if __name__ == "__main__":
    unittest.main()
