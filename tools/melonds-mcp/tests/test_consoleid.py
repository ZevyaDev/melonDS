"""Console identity: emu.getconsoleid(), the process-wide instance base, MAC and save numbering."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from melonds_mcp import native

from .base import have_library

PKG_ROOT = Path(__file__).resolve().parents[1]

# Runs in a fresh process (the instance base is process-wide): prints a JSON report.
SCENARIO = r"""
import json, sys
from melonds_mcp import native
base = int(sys.argv[1]); rom = sys.argv[2]
native.set_instance_base(base)
cons = [native.Console(), native.Console()]
out = {"ids": [int(c.lua_exec("print(emu.getconsoleid())").strip()) for c in cons],
       "macs": [c.mac().hex() for c in cons]}
for c in cons:
    c.load_rom(rom)
out["saves"] = [c.save_path() for c in cons]
print(json.dumps(out))
for c in cons:
    c.close()
"""


def make_rom(path):
    """Smallest .nds ParseROM accepts: a 0x1000-byte image with sane ARM9/ARM7 extents."""
    rom = bytearray(0x1000)
    rom[0:12] = b"IDTEST\0\0\0\0\0\0"
    rom[12:16] = b"ATST"
    struct.pack_into("<IIII", rom, 0x20, 0x200, 0x02000000, 0x02000000, 0x100)
    struct.pack_into("<IIII", rom, 0x30, 0x400, 0x02380000, 0x02380000, 0x100)
    Path(path).write_bytes(rom)


def run_scenario(base, rom):
    env = dict(os.environ)
    env["PYTHONPATH"] = str(PKG_ROOT) + os.pathsep + env.get("PYTHONPATH", "")
    r = subprocess.run([sys.executable, "-c", SCENARIO, str(base), str(rom)],
                       capture_output=True, text=True, env=env, timeout=120)
    if r.returncode != 0:
        raise AssertionError(f"scenario failed ({r.returncode}):\n{r.stdout}\n{r.stderr}")
    return json.loads(r.stdout.strip().splitlines()[-1])


@unittest.skipUnless(have_library(), "melonds_headless library not found")
class ConsoleIdTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.rom = Path(cls.tmp.name) / "synthetic.nds"
        make_rom(cls.rom)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_default_base_ids(self):
        r = run_scenario(0, self.rom)
        self.assertEqual(r["ids"], [0, 1])

    def test_base_offsets_ids(self):
        r = run_scenario(4, self.rom)
        self.assertEqual(r["ids"], [4, 5])

    def test_mac_differs_by_base(self):
        r0 = run_scenario(0, self.rom)
        r4 = run_scenario(4, self.rom)
        self.assertNotEqual(r0["macs"][0], r0["macs"][1])
        self.assertNotEqual(r0["macs"][0], r4["macs"][0])
        self.assertNotEqual(r0["macs"][1], r4["macs"][0])
        # console 1 of base 3 has the same effective id as console 0 of base 4
        r3 = run_scenario(3, self.rom)
        self.assertEqual(r3["macs"][1], r4["macs"][0])

    def test_save_numbering(self):
        sav = str(self.rom.with_suffix(".sav"))
        self.assertEqual(run_scenario(0, self.rom)["saves"], [sav, sav + ".2"])
        self.assertEqual(run_scenario(4, self.rom)["saves"], [sav + ".5", sav + ".6"])

    def test_base_rejected_once_consoles_exist(self):
        c = native.Console()
        try:
            with self.assertRaises(native.MelonError):
                native.set_instance_base(2)
            self.assertEqual(native.instance_base(), 0)
        finally:
            c.close()

    def test_base_negative_rejected(self):
        with self.assertRaises(native.MelonError):
            native.set_instance_base(-1)


if __name__ == "__main__":
    unittest.main()
