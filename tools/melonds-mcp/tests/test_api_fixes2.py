"""The fix/api-quirks-2 fixes (API level 6): interrupts leave no break request armed, the pc of every CPU at a park
is its next instruction, mds_write writes the regions the ARM9 bus refuses bytes for (or fails naming the region),
packets longer than the buffer cannot block mds_packets_take, unknown consoles are errors for mds_frames and
mds_destroy, Lua hooks are tagged and protected in the point list (with a total count), and a GDB restart is an
event the C API sees."""
import ctypes
import time
import unittest
from ctypes import c_int, c_uint8

from melonds_mcp import native
from melonds_mcp.native import MelonError

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest
from .test_api_fixes import adds, dma_program


def spin(base):
    a = A.Asm(base)
    a.label("spin")
    a.b("spin")
    return a


class InterruptLeavesNothingArmed(ConsoleTest):
    """mds_interrupt asks both CPUs to break; only one parks, and the other's request is dropped at the park."""

    def setUp(self):
        super().setUp()
        self.load_program(spin(CODE9))
        self.load_program(spin(CODE7), CODE7, cpu=7)

    def test_resume_after_an_interrupt_runs(self):
        self.c.start()
        time.sleep(0.05)
        self.c.interrupt()
        timeout, states = self.c.wait(10000)
        self.assertEqual((timeout, states), (False, [native.PARKED]))
        self.assertEqual(self.c.take_break().kind, "request")
        # the second CPU's request used to park this run again at once
        self.assertEqual(self.c.run(3), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())

    def test_interrupting_a_stopped_console_does_nothing(self):
        self.c.interrupt()                                  # idle
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())
        bp = self.c.add_breakpoint(CODE9)
        self.assertEqual(self.c.run(2)[1], [native.PARKED])
        self.c.take_break()
        self.c.remove_point(bp)
        self.c.interrupt()                                  # parked
        self.assertEqual(self.c.run(2), (False, [native.IDLE]))
        self.assertIsNone(self.c.take_break())

    def test_steps_survive_the_cleanup(self):
        self.c.step(3)
        self.assertEqual(self.c.run(2)[1], [native.PARKED])
        self.assertEqual(self.c.take_break().kind, "step")


class GroupLeavesNothingArmed(unittest.TestCase):
    """mds_run/mds_wait interrupt the other consoles of a group when one parks; their leftover requests are
    dropped too, so the next run of the group runs."""

    def test_group_resume_runs(self):
        a, b = native.Console(), native.Console()
        self.addCleanup(a.close)
        self.addCleanup(b.close)
        for c in (a, b):
            c.reset()
            c.write(CODE9, spin(CODE9).assemble())
            c.set_reg("pc", CODE9)
        bp = a.add_breakpoint(CODE9)
        _, states = native.run_group([a, b], 1000, 10000)
        self.assertEqual(states[0], native.PARKED)
        a.take_break()
        b.take_break()
        a.remove_point(bp)
        self.assertEqual(native.run_group([a, b], 3, 10000), (False, [native.IDLE, native.IDLE]))
        self.assertIsNone(a.take_break())
        self.assertIsNone(b.take_break())


class PcIsTheNextInstruction(ConsoleTest):
    """mds_get_regs index 15 is the address of the instruction each CPU executes next, at every park and for both
    CPUs (it was the debugger's last hooked pc: 0 for a CPU without hooks, one behind for one with hooks)."""

    def setUp(self):
        super().setUp()
        self.load_program(adds(16))
        self.load_program(adds(4096, CODE7), CODE7, cpu=7)
        self.c.set_reg("r1", 0)
        self.c.set_reg("r1", 0, cpu=7)

    def park_at_arm9(self, addr):
        self.c.add_breakpoint(addr)
        _, states = self.c.run(5)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.cpu, b.pc), ("exec", 9, addr))

    def check_arm7(self):
        r = self.c.regs(7)
        self.assertEqual(r["pc"], r["r15_raw"] - 4)
        self.assertEqual(r["pc"], CODE7 + 4 * r["r1"])      # r1 adds have run: the next one is r1 + 1
        return r["pc"]

    def test_breaking_cpu(self):
        self.park_at_arm9(CODE9 + 12)
        r = self.c.regs()
        self.assertEqual((r["pc"], r["r1"]), (CODE9 + 12, 3))

    def test_cpu_without_hooks(self):
        self.park_at_arm9(CODE9 + 12)
        self.assertNotEqual(self.check_arm7(), 0)

    def test_cpu_with_hooks_is_not_one_behind(self):
        self.c.add_breakpoint(0x02FF0000, cpu=7)           # never reached: arms the ARM7's hooks
        self.c.trace(64, cpu=7)
        self.park_at_arm9(CODE9 + 12)
        self.c.take_trace()
        pc = self.check_arm7()
        # the pc reported is the next instruction the ARM7 runs
        self.c.step(1, cpu=7)
        self.assertEqual(self.c.run(5)[1], [native.PARKED])
        trace = [t for t in self.c.take_trace() if t[1] == 7]
        self.assertEqual(trace[0][2], pc)

    def test_after_a_pc_change(self):
        self.park_at_arm9(CODE9 + 12)
        self.c.set_reg("pc", CODE9 + 40)
        self.assertEqual(self.c.regs()["pc"], CODE9 + 40)

    def test_idle(self):
        self.assertEqual(self.c.regs()["pc"], CODE9)
        self.assertEqual(self.c.regs(7)["pc"], CODE7)

    def test_dma_break(self):
        src, dst = DATA + 0x500, DATA + 0x600
        prog = dma_program(src, dst)
        done = prog.labels["done"]
        self.load_program(prog)
        self.c.add_watchpoint(dst, 16, write=True)
        _, states = self.c.run(3)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual(b.pc, native.DMA_TAG)               # the event: the DMA tag, no instruction
        r = self.c.regs()
        # the CPUs are between two instructions: the ARM9's next is the one after the store that started the DMA
        self.assertEqual(r["pc"], r["r15_raw"] - 4)
        self.assertEqual(r["pc"], done)
        self.check_arm7()


class FrameBoundaryRequestPc(ConsoleTest):
    def test_request_at_a_frame_boundary_reports_an_instruction(self):
        self.load_program(spin(CODE9))
        self.c.input_mode("wait")
        self.c.start(100)
        deadline = time.time() + 10
        while not self.c.input_waiting():
            self.assertLess(time.time(), deadline)
            time.sleep(0.01)
        self.c.interrupt()
        _, states = self.c.wait(10000)
        self.assertEqual(states, [native.PARKED])
        b = self.c.take_break()
        self.assertEqual((b.kind, b.cpu), ("request", 9))
        r = self.c.regs()
        self.assertEqual(b.pc, r["pc"])
        self.assertEqual(b.pc, CODE9)                        # the spin loop, not R15 (CODE9 + 8)
        self.c.input_mode("repeat")
        self.c.stop_after(0)
        self.c.wait(10000)


POWCNT1 = 0x04000304
VRAMCNT_A = 0x04000240


class WriteRegions(ConsoleTest):
    """mds_write writes palette, VRAM and OAM on the ARM9 bus (which ignores 8-bit writes there) with halfwords,
    reads every write back, and fails naming the region where it cannot write."""

    def setUp(self):
        super().setUp()
        a = A.Asm(CODE9)
        a.ldr_const(0, POWCNT1)
        a.ldr_const(1, 0x820F)                  # both 2D engines and the LCDs on
        a.strh(1, 0, 0)
        a.ldr_const(0, VRAMCNT_A)
        a.mov(1, 0x80)                          # bank A enabled, LCDC (0x06800000)
        a.strb(1, 0, 0)
        a.label("spin")
        a.b("spin")
        self.load_program(a)
        self.run_frames(1)

    def test_palette_vram_oam(self):
        for addr in (0x05000000, 0x05000400, 0x06800000, 0x07000000):
            with self.subTest(hex(addr)):
                self.c.write(addr, bytes(range(1, 9)))
                self.assertEqual(self.c.read(addr, 8), bytes(range(1, 9)))

    def test_odd_edges_keep_their_neighbours(self):
        addr = 0x05000010
        self.c.write(addr, bytes([0xAA] * 8))
        self.c.write(addr + 1, bytes([1, 2, 3]))     # odd start, odd end
        self.assertEqual(self.c.read(addr, 8), bytes([0xAA, 1, 2, 3, 0xAA, 0xAA, 0xAA, 0xAA]))
        self.c.write(addr + 7, bytes([0x55]))        # one byte
        self.assertEqual(self.c.read(addr + 6, 2), bytes([0xAA, 0x55]))

    def test_unmapped_vram_fails(self):
        with self.assertRaises(MelonError) as cm:
            self.c.write(0x06820000, bytes([0x12, 0x34]))      # bank B is not mapped
        self.assertIn("VRAM at 0x06820000 did not take the write", str(cm.exception))

    def test_io_and_rom_fail_with_the_region(self):
        cases = [(0x04000000, "I/O at 0x04000000 cannot be written"),
                 (0x08000000, "the GBA ROM area at 0x08000000 is read-only"),
                 (0x09FFFFFE, "the GBA ROM area at 0x09FFFFFE is read-only"),
                 (0xFFFF0000, "ARM9 BIOS at 0xFFFF0000 is read-only")]
        for addr, text in cases:
            with self.subTest(hex(addr)):
                with self.assertRaises(MelonError) as cm:
                    self.c.write(addr, bytes([1, 2]))
                self.assertIn(text, str(cm.exception))
        with self.assertRaises(MelonError) as cm:
            self.c.write(0x00000000, bytes([1]), cpu=7)
        self.assertIn("ARM7 BIOS at 0x00000000 is read-only", str(cm.exception))

    def test_main_ram_still_takes_bytes(self):
        self.c.write(DATA + 3, bytes([1, 2, 3]))
        self.assertEqual(self.c.read(DATA + 3, 3), bytes([1, 2, 3]))
        self.c.write(DATA + 0x103, bytes([9]), cpu=7)
        self.assertEqual(self.c.read(DATA + 0x103, 1, cpu=7), bytes([9]))

    def test_lua_writes_the_same_way(self):
        out = self.c.lua_exec(
            "memory.write_u8(0x05000021, 0x5A, 'arm9'); memory.write_u16_le(0x07000040, 0x1234)\n"
            "memory.write_bytes_as_array(0x06800011, {7, 8, 9}, 'arm9')\n"
            "print(memory.read_u8(0x05000021, 'arm9'), memory.read_u16_le(0x07000040),"
            " memory.read_u8(0x06800013, 'arm9'))")
        self.assertEqual(out, "90\t4660\t9\n")
        with self.assertRaises(MelonError) as cm:
            self.c.lua_exec("memory.write_u32_le(0x04000208, 1)")
        self.assertIn("memory write failed: I/O at 0x04000208", str(cm.exception))


class PacketsNeverBlock(ConsoleTest):
    """A staged packet longer than max_data used to be returned by no call: every mds_packets_take returned 0 and
    the packets behind it were stuck. The first packet of a call is now cut to max_data (length < total)."""

    def log(self, size, fill):
        data = (c_uint8 * size)(*([fill] * size))
        self.assertEqual(native.lib().mds_debug_packet(self.c.id, 1, 0, data, size), 0)

    def take(self, max_packets, max_data):
        hdrs, data = (native.MdsPacket * max_packets)(), (c_uint8 * max(max_data, 1))()
        n = native.lib().mds_packets_take(self.c.id, hdrs, data, max_packets, max_data)
        self.assertGreaterEqual(n, 0)
        out, off = [], 0
        for h in hdrs[:n]:
            out.append((h.length, h.total, bytes(data[off:off + h.length])))
            off += h.length
        return out

    def test_long_packet_is_cut_short_and_the_rest_follow(self):
        self.c.packet_log(True)
        self.log(100, 1)
        self.log(10, 2)
        self.log(10, 3)
        self.assertEqual(self.take(8, 32), [(32, 100, bytes([1] * 32))])
        self.assertEqual(self.take(8, 32), [(10, 10, bytes([2] * 10)), (10, 10, bytes([3] * 10))])
        self.assertEqual(self.take(8, 32), [])

    def test_a_packet_that_does_not_fit_after_others_waits(self):
        self.c.packet_log(True)
        self.log(10, 1)
        self.log(30, 2)
        self.assertEqual(self.take(8, 32), [(10, 10, bytes([1] * 10))])
        self.assertEqual(self.take(8, 32), [(30, 30, bytes([2] * 30))])

    def test_wrapper_reports_the_size(self):
        self.c.packet_log(True)
        self.log(4, 7)
        self.assertEqual(self.c.take_packets(), [{"timestamp": 0, "sent": True, "type": 0, "data": bytes([7] * 4)}])


class UnknownIds(unittest.TestCase):
    def test_frames_of_an_unknown_console_is_an_error(self):
        lib = native.lib()
        self.assertEqual(lib.mds_frames(9999), native.FRAMES_ERROR)
        self.assertIn("no console 9999", lib.mds_error().decode())
        c = native.Console()
        c.reset()
        self.addCleanup(c.close)
        self.assertEqual(c.frames, 0)          # a fresh console is 0, not the error
        c.run(2)
        self.assertEqual(c.frames, 2)

    def test_destroy_of_an_unknown_console_is_an_error(self):
        lib = native.lib()
        self.assertEqual(lib.mds_destroy(9999), -1)
        self.assertIn("no console 9999", lib.mds_error().decode())
        c = native.Console()
        i = c.id
        self.assertEqual(lib.mds_destroy(i), 0)
        c.id = None
        self.assertEqual(lib.mds_destroy(i), -1)       # already gone
        with self.assertRaises(MelonError):
            native.Console.frames.fget(type("C", (), {"id": i})())


class PointList(ConsoleTest):
    def test_lua_hooks_are_tagged_and_protected(self):
        bp = self.c.add_breakpoint(CODE9 + 0x40)
        self.c.lua_run("hook = event.onmemorywrite(function() end, 0x%x, 'w', 'arm9', 4)\n"
                       "xhook = event.onmemoryexecute(function() end, 0x%x)" % (DATA, CODE9 + 0x80))
        points = {p["id"]: p for p in self.c.points()}
        self.assertEqual(len(points), 3)
        self.assertFalse(points[bp]["lua"])
        hooks = [p for p in points.values() if p["lua"]]
        self.assertEqual(sorted(p["kind"] for p in hooks), ["exec", "watch"])
        watch = next(p for p in hooks if p["kind"] == "watch")
        self.assertTrue(watch["write"])                      # the tag does not hide the watch flags
        for p in hooks:
            with self.subTest(p["id"]):
                with self.assertRaises(MelonError) as cm:
                    self.c.remove_point(p["id"])
                self.assertIn(f"point {p['id']} is a Lua hook", str(cm.exception))
        self.c.remove_point(bp)                              # an ordinary point still goes
        self.assertEqual(self.c.point_count(), 2)
        # the script removes its own hooks
        self.c.lua_exec("event.unregisterbyid(hook); event.unregisterbyid(xhook)")
        self.assertEqual(self.c.points(), [])

    def test_wifi_hooks_cannot_be_removed_from_outside(self):
        self.c.lua_run("wh = wifi.hook(function() end)")
        wid = int(self.c.lua_exec("print(wh)").strip())
        with self.assertRaises(MelonError) as cm:
            self.c.remove_point(wid)
        self.assertIn("is a Lua hook", str(cm.exception))

    def test_count_and_truncation(self):
        lib = native.lib()
        for i in range(10):
            self.c.add_breakpoint(CODE9 + 0x100 + 4 * i)
        self.c.add_watchpoint(DATA, 4)
        self.assertEqual(lib.mds_point_count(self.c.id), 11)
        buf = (native.MdsPoint * 4)()
        self.assertEqual(lib.mds_point_list(self.c.id, buf, 4), 4)    # still the number copied
        self.assertEqual(len(self.c.points()), 11)
        self.assertEqual(lib.mds_point_count(9999), -1)


if __name__ == "__main__":
    unittest.main()
