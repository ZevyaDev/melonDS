import unittest

from . import armasm as A
from .base import CODE7, CODE9, DATA, ConsoleTest


def counter_program(base, counter):
    a = A.Asm(base)
    a.ldr_const(0, counter)
    a.label("loop")
    a.ldr(1, 0)
    a.add(1, 1, 1)
    a.str(1, 0)
    a.b("loop")
    return a


class Smoke(ConsoleTest):
    def test_arm9_counter(self):
        self.load_program(counter_program(CODE9, DATA))
        self.run_frames(2)
        self.assertGreater(self.word(DATA), 1000)

    def test_arm7_counter_from_main_ram(self):
        self.load_program(counter_program(CODE7, DATA + 0x100), CODE7, cpu=7)
        self.run_frames(2)
        self.assertGreater(self.word(DATA + 0x100), 1000)

    def test_arm7_counter_from_wram(self):
        self.load_program(counter_program(0x03800000, DATA + 0x200), 0x03800000, cpu=7)
        self.run_frames(2)
        self.assertGreater(self.word(DATA + 0x200), 1000)

    def test_thumb_counter(self):
        # ldr r0,[pc,#4]; loop: ldr r1,[r0]; add r1,#1; str r1,[r0]; b loop; (pad); .word DATA
        code = A.thumb(A.t_ldr_pc(0, 2), A.t_ldr(1, 0), A.t_add(1, 1), A.t_str(1, 0), A.t_b(-5), 0)
        code += DATA.to_bytes(4, "little")
        self.c.write(CODE9, code)
        # enter Thumb via bx from a one-instruction ARM trampoline
        a = A.Asm(CODE9 + 0x100)
        a.ldr_const(0, CODE9 | 1)
        a.bx(0)
        self.c.write(CODE9 + 0x100, a.assemble())
        self.c.set_reg("pc", CODE9 + 0x100)
        self.run_frames(2)
        self.assertGreater(self.word(DATA), 100)


if __name__ == "__main__":
    unittest.main()
