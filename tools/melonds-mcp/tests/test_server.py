"""Socket server: session, console lifecycle, execution, memory access, disconnect handling."""
import os
import struct
import time

from melonds_mcp import native
from melonds_mcp.client import (BreakEvent, Client, ConsoleGoneEvent, InputQueueEvent, ServerError, T,
                                OPT_DEFAULT_CONSOLE)

from . import armasm as A
from .base import CODE7, CODE9, DATA
from .server_base import ServerTest

COUNTER = DATA + 0x100


def counter_program(base=CODE9, addr=COUNTER):
    a = A.Asm(base)
    a.ldr_const(0, addr)
    a.label("loop")
    a.ldr(1, 0)
    a.add(1, 1, 1)
    a.str(1, 0)
    a.b("loop")
    return a.assemble()


class SessionTests(ServerTest):
    def test_hello_lists_consoles_and_port(self):
        cl = self.connect()
        self.assertEqual(self.info.version, 1)
        self.assertEqual(self.info.port, self.port)
        self.assertEqual(self.info.pid, os.getpid())
        self.assertEqual(self.info.ids, [self.c.id])
        self.assertEqual(self.info.default_console, self.c.id)

    def test_server_name(self):
        native.serve_stop()
        self.port = native.serve(0, name="p2")
        cl = self.connect()
        self.assertEqual(self.info.name, "p2")

    def test_default_console_is_lowest_id(self):
        c2 = self.new_console()
        cl = self.connect()
        self.assertEqual(sorted(self.info.ids), [self.c.id, c2.id])
        self.assertEqual(self.info.default_console, min(self.c.id, c2.id))
        cl.request(T.read, struct.pack("<BII", 9, DATA, 4))
        self.assertEqual(cl.last_console, min(self.c.id, c2.id))

    def test_explicit_console_echoed(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.read(DATA, 4, console=c2.id)
        self.assertEqual(cl.last_console, c2.id)

    def test_unknown_console(self):
        cl = self.connect()
        self.expect_error("no_console", cl.read, DATA, 4, console=9)

    def test_pinned_default_destroyed_gives_no_console(self):
        c2 = self.new_console()
        cl = self.connect(default_console=c2.id)
        cl.read(DATA, 4)
        self.assertEqual(cl.last_console, c2.id)
        gone = c2.id
        c2.close()
        ev = cl.next_event("console_gone")
        self.assertIsNotNone(ev)
        self.assertEqual(ev.console, gone)
        self.expect_error("no_console", cl.read, DATA, 4)
        # an explicit console still works
        cl.read(DATA, 4, console=self.c.id)

    def test_hello_pinned_to_missing_console_fails(self):
        cl = self.connect(hello=False)
        self.expect_error("no_console", cl.hello, default_console=12)

    def test_bad_protocol_version(self):
        cl = self.connect(hello=False)
        self.expect_error("bad_payload", cl.hello, version=99)

    def test_requests_work_without_hello(self):
        cl = self.connect(hello=False)
        self.assertEqual(len(cl.read(DATA, 8)), 8)

    def test_second_client_gets_busy(self):
        cl = self.connect()
        second = Client(self.port, timeout=10)
        self.clients.append(second)
        err = self.expect_error("busy", second.hello)
        # the first client is unaffected
        self.assertEqual(len(cl.read(DATA, 4)), 4)

    def test_set_option_bad_key_and_value(self):
        cl = self.connect()
        self.expect_error("bad_option", cl.set_option, 99, 1)
        self.expect_error("bad_option", cl.set_option, 1, 3)
        self.expect_error("bad_option", cl.set_option, 3, 12)
        self.expect_error("bad_option", cl.set_option, 2, 70000)

    def test_set_option_default_console(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.set_default_console(c2.id)
        cl.read(DATA, 4)
        self.assertEqual(cl.last_console, c2.id)
        cl.set_option(OPT_DEFAULT_CONSOLE, 0xFFFF)
        cl.read(DATA, 4)
        self.assertEqual(cl.last_console, min(self.c.id, c2.id))

    def test_unknown_type(self):
        cl = self.connect()
        self.expect_error("unknown_type", cl.request, 0x7F01, b"")

    def test_unimplemented_messages_not_supported(self):
        cl = self.connect()
        for t in (T.input_multi,):          # plan step 7
            self.expect_error("not_supported", cl.request, t, b"")

    def test_malformed_body(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.request, T.read, b"\x01")


class LifecycleTests(ServerTest):
    def test_create_list_destroy_and_gone(self):
        cl = self.connect()
        new = cl.console_create()
        self.assertNotIn(new, [self.c.id])
        listed = {c.id: c for c in cl.console_list()}
        self.assertEqual(set(listed), {self.c.id, new})
        self.assertEqual(listed[new].state, "idle")
        self.assertEqual(listed[new].frames, 0)
        cl.console_destroy(new)
        ev = cl.next_event("console_gone")
        self.assertEqual(ev.console, new)
        self.assertEqual({c.id for c in cl.console_list()}, {self.c.id})
        self.expect_error("no_console", cl.console_destroy, new)

    def test_console_list_reports_state_and_frames(self):
        cl = self.connect()
        cl.run(7)
        info = {c.id: c for c in cl.console_list()}[self.c.id]
        self.assertEqual(info.state, "idle")
        self.assertEqual(info.frames, 7)

    def test_create_with_bad_bios_fails(self):
        cl = self.connect()
        self.expect_error("failed", cl.console_create, bios9="no-such-file.bin")

    def test_consoles_created_later_get_hello_options(self):
        cl = self.connect(empty_queue="wait")
        new = cl.console_create()
        # in wait mode with an empty queue the console waits for input instead of running
        cl.live_start(0, [new])
        self.wait_until(lambda: native.lib().mds_input_waiting(new) == 1, what="console to wait for input")
        cl.live_stop([new])
        self.wait_until(lambda: native.lib().mds_state(new) == native.IDLE, what="console to stop")
        cl.console_destroy(new)

    def test_rom_load_missing_file(self):
        cl = self.connect()
        self.expect_error("failed", cl.rom_load, "no-such-rom.nds")

    def test_reset_clears_input_queue(self):
        cl = self.connect()
        for _ in range(3):
            cl.input(buttons=["A"])
        self.assertEqual(self.c.input_depth(), 3)
        cl.reset()
        ev = cl.next_event("input_queue")
        self.assertIsNotNone(ev)
        self.assertEqual((ev.console, ev.depth), (self.c.id, 0))
        self.assertEqual(self.c.input_depth(), 0)

    def test_reset_running_console_fails(self):
        cl = self.connect()
        cl.live_start()
        self.expect_error("console_running", cl.reset)
        cl.live_stop()

    def test_shutdown_signals_owner(self):
        cl = self.connect()
        self.assertEqual(native.serve_wait(0), 0)
        cl.shutdown()
        self.assertEqual(native.serve_wait(2000), 1)


class ExecutionTests(ServerTest):
    def setUp(self):
        super().setUp()
        self.c.write(CODE9, counter_program())
        self.c.set_reg("pc", CODE9)

    def test_run_replies_when_done(self):
        cl = self.connect()
        cl.run(10)
        self.assertEqual(self.c.frames, 10)
        self.assertEqual(self.c.state, native.IDLE)
        self.assertGreater(self.word(COUNTER), 0)

    def test_run_unknown_console(self):
        cl = self.connect()
        self.expect_error("no_console", cl.run, 1, [77])

    def test_run_running_console(self):
        cl = self.connect()
        cl.live_start()
        self.expect_error("console_running", cl.run, 5)
        cl.live_stop()

    def test_run_reply_deferred_while_other_console_answers(self):
        c2 = self.new_console()
        cl = self.connect()
        native.set_pacing(60)
        pending = cl.run(30, [self.c.id], wait=False)
        time.sleep(0.05)
        self.assertEqual(len(cl.read(DATA, 4, console=c2.id)), 4)
        self.assertFalse(pending.ready(), "run replied before the console finished")
        pending.result(10)
        self.assertEqual(self.c.frames, 30)

    def test_run_group_waits_for_all(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.run(12, [self.c.id, c2.id])
        self.assertEqual((self.c.frames, c2.frames), (12, 12))

    def test_run_all_consoles_when_list_empty(self):
        c2 = self.new_console()
        cl = self.connect()
        cl.run(4)
        self.assertEqual((self.c.frames, c2.frames), (4, 4))

    def test_run_parks_at_breakpoint_and_interrupts_others(self):
        c2 = self.new_console()
        self.c.add_breakpoint(CODE9 + 4)
        cl = self.connect()
        native.set_pacing(60)
        cl.run(100000, [self.c.id, c2.id])
        ev = cl.next_event("break")
        self.assertIsNotNone(ev)
        self.assertEqual(ev.console, self.c.id)
        self.assertEqual(ev.what, "exec")
        self.assertEqual(ev.pc, CODE9 + 4)
        self.assertEqual(ev.cpu, 9)
        self.assertEqual(self.c.state, native.PARKED)
        self.assertNotEqual(c2.state, native.RUNNING)
        regs = cl.registers(9, console=self.c.id)
        self.assertEqual(regs["pc"], CODE9 + 4)

    def test_live_start_and_stop(self):
        cl = self.connect()
        cl.live_start(0)
        self.wait_until(lambda: self.c.frames > 5, what="frames")
        cl.live_stop()
        self.wait_until(lambda: self.c.state == native.IDLE, what="idle")
        n = self.c.frames
        time.sleep(0.05)
        self.assertEqual(self.c.frames, n)

    def test_live_start_sets_pacing(self):
        cl = self.connect()
        cl.live_start(30)
        time.sleep(0.5)
        cl.live_stop()
        self.wait_until(lambda: self.c.state == native.IDLE)
        self.assertLess(self.c.frames, 40)
        self.assertGreater(self.c.frames, 5)

    def test_pause_stops_at_frame_boundary(self):
        cl = self.connect()
        cl.live_start()
        self.wait_until(lambda: self.c.frames > 3)
        cl.pause()
        self.assertEqual(self.c.state, native.IDLE)
        n = self.c.frames
        time.sleep(0.05)
        self.assertEqual(self.c.frames, n)

    def test_step_executes_instructions_then_stops(self):
        cl = self.connect()
        start = self.c.regs()["pc"]
        cl.step(3, 9)
        self.assertEqual(self.c.state, native.PARKED)
        ev = cl.next_event("break")
        self.assertIsNotNone(ev)
        self.assertEqual(ev.what, "step")
        self.assertEqual(cl.registers()["pc"], self.c.regs()["pc"])

    def test_step_running_console_fails(self):
        cl = self.connect()
        cl.live_start()
        self.expect_error("console_running", cl.step, 1, 9)
        cl.live_stop()


class MemoryTests(ServerTest):
    def test_write_then_read_round_trip(self):
        cl = self.connect()
        data = bytes(range(64))
        cl.write(DATA + 0x400, data)
        self.assertEqual(cl.read(DATA + 0x400, 64), data)
        self.assertEqual(self.c.read(DATA + 0x400, 64), data)

    def test_arm7_read_write(self):
        cl = self.connect()
        cl.write(DATA + 0x800, b"\x11\x22\x33\x44", cpu=7)
        self.assertEqual(cl.read(DATA + 0x800, 4, cpu=7), b"\x11\x22\x33\x44")

    def test_read_bad_cpu(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.request, T.read, struct.pack("<BII", 5, DATA, 4))

    def test_read_too_long(self):
        cl = self.connect()
        self.expect_error("bad_payload", cl.request, T.read, struct.pack("<BII", 9, DATA, 0x2000000))

    def test_write_unmapped(self):
        cl = self.connect()
        self.expect_error("failed", cl.write, 0x04000000, b"\x00", cpu=7)     # ARM7 IO space is not pokeable

    def test_registers_and_set_register(self):
        cl = self.connect()
        cl.set_register("r5", 0xCAFEBABE)
        self.assertEqual(cl.registers()["r5"], 0xCAFEBABE)
        self.assertEqual(self.c.regs()["r5"], 0xCAFEBABE)
        cl.set_register("pc", CODE9)
        self.assertEqual(cl.registers()["pc"], CODE9)

    def test_set_register_out_of_range(self):
        cl = self.connect()
        self.expect_error("failed", cl.set_register, 16, 0)

    def test_registers_need_stopped_console(self):
        cl = self.connect()
        cl.live_start()
        self.expect_error("console_running", cl.registers)
        self.expect_error("console_running", cl.set_register, "r1", 1)
        cl.live_stop()

    def test_read_during_live_play_sees_whole_frames(self):
        # the counter program increments a word forever; reads come from the frame boundary
        self.c.write(CODE9, counter_program())
        self.c.set_reg("pc", CODE9)
        cl = self.connect()
        cl.live_start()
        a = struct.unpack("<I", cl.read(COUNTER, 4))[0]
        b = struct.unpack("<I", cl.read(COUNTER, 4))[0]
        cl.live_stop()
        self.assertGreater(b, a)

    def test_program_runs_and_is_visible_through_socket(self):
        self.c.write(CODE9, counter_program())
        self.c.set_reg("pc", CODE9)
        cl = self.connect()
        cl.run(3)
        self.assertGreater(struct.unpack("<I", cl.read(COUNTER, 4))[0], 0)

    def test_screenshot(self):
        cl = self.connect()
        cl.run(3)
        frame, pixels = cl.screenshot()
        self.assertEqual(frame, 3)
        self.assertEqual(len(pixels), 256 * 384 * 4)
        native_pixels = self.c.screen()
        self.assertEqual(pixels, bytes(native_pixels))


class DisconnectTests(ServerTest):
    def test_disconnect_stops_live_consoles(self):
        cl = self.connect()
        cl.live_start()
        self.wait_until(lambda: self.c.frames > 2)
        cl.close()
        self.wait_until(lambda: self.c.state == native.IDLE, what="console to stop")

    def test_disconnect_clears_input_queues(self):
        cl = self.connect()
        for _ in range(5):
            cl.input(buttons=["A"])
        self.assertEqual(self.c.input_depth(), 5)
        cl.close()
        self.wait_until(lambda: self.c.input_depth() == 0, what="queue to clear")

    def test_options_reset_for_next_client(self):
        cl = self.connect(empty_queue="wait")
        cl.close()
        time.sleep(0.1)
        cl2 = self.connect(hello=False)
        # without a hello the defaults apply: repeat mode runs without input
        timed_out, states = self.c.run(3, timeout_ms=5000)
        self.assertFalse(timed_out)
        self.assertEqual(self.c.frames, 3)

    def test_server_accepts_next_client(self):
        cl = self.connect()
        cl.close()
        time.sleep(0.05)
        cl2 = self.connect()
        self.assertEqual(cl2.read(DATA, 4), cl2.read(DATA, 4))

    def test_consoles_keep_state_across_clients(self):
        cl = self.connect()
        cl.write(DATA + 0x500, b"keep")
        cl.close()
        time.sleep(0.05)
        cl2 = self.connect()
        self.assertEqual(cl2.read(DATA + 0x500, 4), b"keep")

    def test_subscriptions_dropped_on_disconnect(self):
        cl = self.connect()
        cl.subscribe(1, [(9, DATA, 16)])
        cl.close()
        time.sleep(0.1)
        cl2 = self.connect()
        cl2.run(5)
        self.assertEqual(cl2.take_events("frame"), [])

    def test_stop_while_connected(self):
        cl = self.connect()
        native.serve_stop()
        with self.assertRaises(Exception):
            cl.read(DATA, 4)
        self.port = native.serve(0)     # restartable
        cl2 = self.connect()
        self.assertEqual(len(cl2.read(DATA, 4)), 4)

    def test_serve_twice_is_an_error(self):
        with self.assertRaises(native.MelonError):
            native.serve(0)

    def test_no_free_port(self):
        import socket
        native.serve_stop()
        blocker = socket.socket()
        self.addCleanup(blocker.close)
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(1)
        taken = blocker.getsockname()[1]
        with self.assertRaises(native.MelonError) as cm:
            native.serve(taken, 1)
        self.assertIn("no free port", str(cm.exception))
        self.port = native.serve(0)
