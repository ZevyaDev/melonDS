import capstone

_arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
_thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
for _cs in (_arm, _thumb):
    _cs.detail = False

BRANCHES = ("b", "bl", "blx", "beq", "bne", "bcs", "bcc", "bmi", "bpl", "bvs", "bvc", "bhi", "bls",
            "bge", "blt", "bgt", "ble", "bhs", "blo")


def disassemble(code, addr, thumb, describe=None):
    cs = _thumb if thumb else _arm
    out = []
    pos = 0
    step = 2 if thumb else 4
    while pos < len(code):
        insns = list(cs.disasm(code[pos:], addr + pos, 1))
        if not insns:
            word = int.from_bytes(code[pos:pos + step], "little")
            out.append((addr + pos, code[pos:pos + step].hex(), ".word" if step == 4 else ".hword", f"0x{word:x}"))
            pos += step
            continue
        i = insns[0]
        ops = i.op_str
        if describe and i.mnemonic.split(".")[0] in BRANCHES and ops.startswith("#0x"):
            target = int(ops[1:], 16)
            ops = f"{ops} <{describe(target)}>"
        out.append((i.address, i.bytes.hex(), i.mnemonic, ops))
        pos += i.size
    return out


def format_lines(lines, describe=None, mark=None):
    rows = []
    for addr, raw, mnem, ops in lines:
        label = ""
        if describe:
            d = describe(addr)
            if "+" not in d and not d.startswith(f"{addr:08x}"):
                label = f"{d}:\n"
        arrow = "=>" if mark == addr else "  "
        rows.append(f"{label}{arrow} {addr:08x}  {raw:<8}  {mnem} {ops}".rstrip())
    return "\n".join(rows)
