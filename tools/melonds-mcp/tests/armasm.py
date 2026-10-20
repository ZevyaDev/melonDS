"""Tiny ARM / Thumb assembler for synthetic test programs.

Only what the tests need. ARM programs are built with ``Asm``: instructions are
appended in order, labels are resolved at ``assemble()``, and ``ldr_const``
puts 32-bit constants in a literal pool emitted after the code.
"""
import struct

EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL = range(15)

_AND, _EOR, _SUB, _RSB, _ADD, _ADC, _SBC, _RSC, _TST, _TEQ, _CMP, _CMN, _ORR, _MOV, _BIC, _MVN = range(16)


def encode_imm(value):
    """Return (rot, imm8) for an ARM modified immediate, or raise ValueError."""
    value &= 0xFFFFFFFF
    for rot in range(16):
        v = ((value << (2 * rot)) | (value >> (32 - 2 * rot))) & 0xFFFFFFFF if rot else value
        if v < 256:
            return rot, v
    raise ValueError(f"{value:#x} is not an ARM immediate")


def _dp(cond, op, s, rn, rd, op2):
    return (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | op2


def _dp_imm(cond, op, s, rn, rd, imm):
    rot, v = encode_imm(imm)
    return _dp(cond, op, s, rn, rd, (1 << 25) | (rot << 8) | v)


class Asm:
    def __init__(self, base):
        self.base = base
        self.items = []     # ("w", word) | ("b", cond, link, label) | ("ldr", rd, pool_index)
        self.labels = {}
        self.pool = []

    @property
    def pc(self):
        return self.base + 4 * len(self.items)

    def label(self, name):
        self.labels[name] = self.pc

    def word(self, w):
        self.items.append(("w", w & 0xFFFFFFFF))

    # data processing
    def mov(self, rd, imm, cond=AL):
        self.word(_dp_imm(cond, _MOV, 0, 0, rd, imm))

    def mov_reg(self, rd, rm, cond=AL):
        self.word(_dp(cond, _MOV, 0, 0, rd, rm))

    def orr(self, rd, rn, imm, cond=AL):
        self.word(_dp_imm(cond, _ORR, 0, rn, rd, imm))

    def add(self, rd, rn, imm, cond=AL):
        self.word(_dp_imm(cond, _ADD, 0, rn, rd, imm))

    def add_reg(self, rd, rn, rm, cond=AL):
        self.word(_dp(cond, _ADD, 0, rn, rd, rm))

    def sub(self, rd, rn, imm, cond=AL):
        self.word(_dp_imm(cond, _SUB, 0, rn, rd, imm))

    def subs(self, rd, rn, imm, cond=AL):
        self.word(_dp_imm(cond, _SUB, 1, rn, rd, imm))

    def cmp(self, rn, imm, cond=AL):
        self.word(_dp_imm(cond, _CMP, 1, rn, 0, imm))

    def cmp_reg(self, rn, rm, cond=AL):
        self.word(_dp(cond, _CMP, 1, rn, 0, rm))

    def nop(self):
        self.mov_reg(0, 0)

    # memory: word/byte with immediate offset
    def _ldst(self, load, byte, rd, rn, off, cond):
        up = 1 if off >= 0 else 0
        self.word((cond << 28) | (0b01 << 26) | (1 << 24) | (up << 23) | (byte << 22) | (load << 20)
                  | (rn << 16) | (rd << 12) | abs(off))

    def ldr(self, rd, rn, off=0, cond=AL):
        self._ldst(1, 0, rd, rn, off, cond)

    def str(self, rd, rn, off=0, cond=AL):
        self._ldst(0, 0, rd, rn, off, cond)

    def ldrb(self, rd, rn, off=0, cond=AL):
        self._ldst(1, 1, rd, rn, off, cond)

    def strb(self, rd, rn, off=0, cond=AL):
        self._ldst(0, 1, rd, rn, off, cond)

    def _ldsth(self, load, rd, rn, off, cond):
        up = 1 if off >= 0 else 0
        off = abs(off)
        self.word((cond << 28) | (1 << 24) | (up << 23) | (1 << 22) | (load << 20) | (rn << 16) | (rd << 12)
                  | ((off >> 4) << 8) | 0xB0 | (off & 0xF))

    def ldrh(self, rd, rn, off=0, cond=AL):
        self._ldsth(1, rd, rn, off, cond)

    def strh(self, rd, rn, off=0, cond=AL):
        self._ldsth(0, rd, rn, off, cond)

    def ldr_const(self, rd, value):
        """ldr rd, =value via the literal pool."""
        if value not in self.pool:
            self.pool.append(value)
        self.items.append(("ldr", rd, self.pool.index(value)))

    # branches
    def b(self, label, cond=AL):
        self.items.append(("b", cond, 0, label))

    def bl(self, label, cond=AL):
        self.items.append(("b", cond, 1, label))

    def bx(self, rm, cond=AL):
        self.word((cond << 28) | 0x012FFF10 | rm)

    def spin(self, reg, count):
        """Busy-wait of roughly 3*count cycles using ``reg`` as the counter."""
        name = f"_spin{len(self.items)}"
        self.ldr_const(reg, count)
        self.label(name)
        self.subs(reg, reg, 1)
        self.b(name, NE)

    def assemble(self):
        code_words = len(self.items)
        pool_base = self.base + 4 * code_words
        out = []
        for i, it in enumerate(self.items):
            here = self.base + 4 * i
            if it[0] == "w":
                out.append(it[1])
            elif it[0] == "b":
                _, cond, link, label = it
                off = (self.labels[label] - (here + 8)) >> 2
                out.append((cond << 28) | (0b101 << 25) | (link << 24) | (off & 0xFFFFFF))
            else:
                _, rd, idx = it
                off = pool_base + 4 * idx - (here + 8)
                out.append((AL << 28) | (0b01 << 26) | (1 << 24) | (1 << 23) | (1 << 20) | (15 << 16) | (rd << 12) | off)
        out.extend(self.pool)
        return struct.pack(f"<{len(out)}I", *out)


# ---------------------------------------------------------------- Thumb

def t_mov(rd, imm8):
    return 0x2000 | (rd << 8) | imm8


def t_add(rd, imm8):
    return 0x3000 | (rd << 8) | imm8


def t_sub(rd, imm8):
    return 0x3800 | (rd << 8) | imm8


def t_cmp(rd, imm8):
    return 0x2800 | (rd << 8) | imm8


def t_ldr(rd, rn, off=0):
    return 0x6800 | ((off >> 2) << 6) | (rn << 3) | rd


def t_str(rd, rn, off=0):
    return 0x6000 | ((off >> 2) << 6) | (rn << 3) | rd


def t_ldr_pc(rd, word_off):
    """ldr rd, [pc, #word_off*4]; the base is (instr address + 4) & ~3."""
    return 0x4800 | (rd << 8) | word_off


def t_b(halfword_off):
    """Unconditional branch; offset in halfwords relative to (instr + 4)."""
    return 0xE000 | (halfword_off & 0x7FF)


def t_bcond(cond, halfword_off):
    return 0xD000 | (cond << 8) | (halfword_off & 0xFF)


def t_bx(rm):
    return 0x4700 | (rm << 3)


def thumb(*halfwords):
    return struct.pack(f"<{len(halfwords)}H", *halfwords)
