/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <cstring>

#include <QRegularExpression>

#include "Disassembler.h"

#ifdef MELONDS_HAVE_CAPSTONE
#include <capstone/capstone.h>
#endif

using namespace melonDS;

static s32 SignExtend(u32 value, int bits)
{
    u32 m = 1u << (bits - 1);
    value &= (1u << bits) - 1;
    return (s32)((value ^ m) - m);
}

Disassembler::Disassembler()
{
#ifdef MELONDS_HAVE_CAPSTONE
    csh h;
    if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &h) == CS_ERR_OK)
        Arm = new csh(h);
    if (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &h) == CS_ERR_OK)
        Thumb = new csh(h);
#endif
}

Disassembler::~Disassembler()
{
#ifdef MELONDS_HAVE_CAPSTONE
    for (void* p : {Arm, Thumb})
        if (p)
        {
            cs_close((csh*)p);
            delete (csh*)p;
        }
#endif
}

bool Disassembler::available()
{
#ifdef MELONDS_HAVE_CAPSTONE
    return true;
#else
    return false;
#endif
}

static QString PlainRegisterNames(const QString& ops)
{
    static const QRegularExpression apcs("\\b(sb|sl|fp|ip)\\b");
    if (!ops.contains(apcs))
        return ops;
    QString out = ops;
    out.replace(QRegularExpression("\\bsb\\b"), "r9");
    out.replace(QRegularExpression("\\bsl\\b"), "r10");
    out.replace(QRegularExpression("\\bfp\\b"), "r11");
    out.replace(QRegularExpression("\\bip\\b"), "r12");
    return out;
}

static void Analyze(DisasmLine& line, bool thumb)
{
    u32 pc = line.Addr;
    u32 op = line.Opcode;
    if (!thumb)
    {
        u32 cond = op >> 28;
        if ((op & 0x0E000000) == 0x0A000000)
        {
            line.HasTarget = true;
            line.Target = pc + 8 + (SignExtend(op, 24) << 2);
            if (cond == 0xF)
            {
                line.Target += (op >> 23) & 2;
                line.IsCall = true;
            }
            else
                line.IsCall = op & (1 << 24);
        }
        else if (cond != 0xF && (op & 0x0F7F0000) == 0x051F0000)
        {
            u32 imm = op & 0xFFF;
            line.HasTarget = true;
            line.IsLiteral = true;
            line.Target = (op & (1 << 23)) ? pc + 8 + imm : pc + 8 - imm;
        }
        return;
    }
    u16 h = op & 0xFFFF;
    if ((h & 0xF000) == 0xD000 && (h & 0x0F00) < 0x0E00)
    {
        line.HasTarget = true;
        line.Target = pc + 4 + (SignExtend(h, 8) << 1);
    }
    else if ((h & 0xF800) == 0xE000)
    {
        line.HasTarget = true;
        line.Target = pc + 4 + (SignExtend(h, 11) << 1);
    }
    else if ((h & 0xF800) == 0x4800)
    {
        line.HasTarget = true;
        line.IsLiteral = true;
        line.Target = ((pc + 4) & ~3u) + ((h & 0xFF) << 2);
    }
    else if (line.Size == 4)
    {
        u16 lo = op >> 16;
        line.HasTarget = true;
        line.IsCall = true;
        line.Target = pc + 4 + (SignExtend(h, 11) << 12) + ((lo & 0x7FF) << 1);
        if ((lo & 0xF800) == 0xE800)
            line.Target &= ~3u;
    }
}

DisasmLine Disassembler::decode(u32 addr, const u8* bytes, int avail, bool thumb)
{
    DisasmLine line {};
    line.Addr = addr;
    int size = thumb ? 2 : 4;
    if (avail < size)
    {
        line.Size = size;
        line.Mnemonic = "??";
        return line;
    }
    if (thumb)
    {
        u16 first;
        memcpy(&first, bytes, 2);
        line.Opcode = first;
        if ((first & 0xF800) == 0xF000 && avail >= 4)
        {
            u16 second;
            memcpy(&second, bytes + 2, 2);
            if ((second & 0xE800) == 0xE800)
            {
                size = 4;
                line.Opcode = first | ((u32)second << 16);
            }
        }
    }
    else
        memcpy(&line.Opcode, bytes, 4);
    line.Size = size;

    Analyze(line, thumb);

    if (thumb && size == 2 && (line.Opcode & 0xE000) == 0xE000 && (line.Opcode & 0x1800))
    {
        line.Mnemonic = (line.Opcode & 0xF800) == 0xF000 ? "bl.hi" : "bl.lo";
        line.Operands = QString("#0x%1").arg(line.Opcode & 0x7FF, 0, 16);
        return line;
    }
    if (thumb && size == 4)
    {
        line.Mnemonic = ((line.Opcode >> 16) & 0xF800) == 0xE800 ? "blx" : "bl";
        line.Operands = QString("#0x%1").arg(line.Target, 8, 16, QChar('0'));
        return line;
    }

#ifdef MELONDS_HAVE_CAPSTONE
    void* handle = thumb ? Thumb : Arm;
    if (handle)
    {
        cs_insn* insn = nullptr;
        size_t n = cs_disasm(*(csh*)handle, bytes, size, addr, 1, &insn);
        if (n)
        {
            line.Mnemonic = QString::fromLatin1(insn[0].mnemonic);
            line.Operands = PlainRegisterNames(QString::fromLatin1(insn[0].op_str));
            cs_free(insn, n);
            return line;
        }
    }
#endif
    line.Mnemonic = thumb ? ".hword" : ".word";
    line.Operands = QString("0x%1").arg(line.Opcode, thumb ? 4 : 8, 16, QChar('0'));
    return line;
}
