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

#include <cstdio>
#include <string>

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

static InstrKind ClassifyArm(u32 op)
{
    u32 rd = (op >> 12) & 0xF;
    bool load = op & (1 << 20);
    if ((op >> 28) == 0xF)
        return (op & 0x0E000000) == 0x0A000000 ? InstrKind::Call : InstrKind::System;
    if ((op & 0x0E000000) == 0x0A000000)
        return (op & (1 << 24)) ? InstrKind::Call : InstrKind::Branch;
    if ((op & 0x0FFFFFF0) == 0x012FFF30)
        return InstrKind::Call;
    if ((op & 0x0FFFFFF0) == 0x012FFF10)
        return (op & 0xF) == 14 ? InstrKind::Return : InstrKind::Branch;
    if ((op & 0x0C000000) == 0x0C000000)
        return InstrKind::System;
    if ((op & 0x0E000000) == 0x08000000)
    {
        if (load && (op & (1 << 15)))
            return InstrKind::Return;
        return load ? InstrKind::Load : InstrKind::Store;
    }
    if ((op & 0x0C000000) == 0x04000000)
    {
        if (load && rd == 15)
            return ((op >> 16) & 0xF) == 13 ? InstrKind::Return : InstrKind::Branch;
        return load ? InstrKind::Load : InstrKind::Store;
    }
    if ((op & 0x0E000090) == 0x00000090)
    {
        if ((op & 0x60) == 0)
            return (op & 0x0FB00FF0) == 0x01000090 ? InstrKind::Load : InstrKind::Other;
        return (load || (op & 0x60) == 0x40) ? InstrKind::Load : InstrKind::Store;
    }
    if ((op & 0x0F900000) == 0x01000000 && !load)
        return (op & 0x0FFF0FF0) == 0x016F0F10 || (op & 0x0F900090) == 0x01000080 || (op & 0x0F9000F0) == 0x01000050
            ? InstrKind::Other : InstrKind::System;
    if ((op & 0x0FB00000) == 0x03200000)
        return InstrKind::System;
    u32 opcode = (op >> 21) & 0xF;
    if (opcode >= 0x8 && opcode <= 0xB)
        return InstrKind::Compare;
    if (rd == 15)
    {
        bool lr = !(op & (1 << 25)) && (op & 0xFFF) == 14;
        if ((opcode == 0xD && lr) || (load && opcode == 0x2 && ((op >> 16) & 0xF) == 14))
            return InstrKind::Return;
        return InstrKind::Branch;
    }
    return InstrKind::Other;
}

static InstrKind ClassifyThumb(u32 op, u8 size)
{
    u16 h = op & 0xFFFF;
    if (size == 4 || (h & 0xE000) == 0xE000)
        return (h & 0xF800) == 0xE000 ? InstrKind::Branch : InstrKind::Call;
    if ((h & 0xFF87) == 0x4780)
        return InstrKind::Call;
    if ((h & 0xFF87) == 0x4700 || (h & 0xFF87) == 0x4687)
        return ((h >> 3) & 0xF) == 14 ? InstrKind::Return : InstrKind::Branch;
    if ((h & 0xFF87) == 0x4487)
        return InstrKind::Branch;
    if ((h & 0xF000) == 0xD000)
        return (h & 0x0F00) >= 0x0E00 ? InstrKind::System : InstrKind::Branch;
    if ((h & 0xFF00) == 0xBD00)
        return InstrKind::Return;
    if ((h & 0xF600) == 0xB400)
        return (h & 0x0800) ? InstrKind::Load : InstrKind::Store;
    if ((h & 0xF800) == 0x4800)
        return InstrKind::Load;
    if ((h & 0xF000) == 0x5000)
        return (h & 0x0E00) <= 0x0400 ? InstrKind::Store : InstrKind::Load;
    if ((h & 0xE000) == 0x6000 || (h & 0xE000) == 0x8000 || (h & 0xF000) == 0xC000)
        return (h & 0x0800) ? InstrKind::Load : InstrKind::Store;
    if ((h & 0xF800) == 0x2800 || (h & 0xFF00) == 0x4500 || (h & 0xFFC0) == 0x4200 || (h & 0xFF80) == 0x4280)
        return InstrKind::Compare;
    if ((h & 0xFF00) == 0xBE00)
        return InstrKind::System;
    return InstrKind::Other;
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

static bool IsWordChar(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

// Capstone prints the APCS names sb/sl/fp/ip; show plain r9-r12 instead
static std::string PlainRegisterNames(const std::string& ops)
{
    static const struct { const char* Name; const char* Plain; } map[] = {
        {"sb", "r9"}, {"sl", "r10"}, {"fp", "r11"}, {"ip", "r12"}};

    std::string out;
    size_t i = 0;
    while (i < ops.size())
    {
        if (!IsWordChar(ops[i]))
        {
            out += ops[i++];
            continue;
        }
        size_t j = i;
        while (j < ops.size() && IsWordChar(ops[j]))
            j++;
        std::string word = ops.substr(i, j - i);
        for (auto& m : map)
            if (word == m.Name)
                word = m.Plain;
        out += word;
        i = j;
    }
    return out;
}

static std::string Format(const char* fmt, u32 value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, value);
    return buf;
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
    line.Kind = thumb ? ClassifyThumb(line.Opcode, size) : ClassifyArm(line.Opcode);

    if (thumb && size == 2 && (line.Opcode & 0xE000) == 0xE000 && (line.Opcode & 0x1800))
    {
        line.Mnemonic = (line.Opcode & 0xF800) == 0xF000 ? "bl.hi" : "bl.lo";
        line.Operands = Format("#0x%x", line.Opcode & 0x7FF);
        return line;
    }
    if (thumb && size == 4)
    {
        line.Mnemonic = ((line.Opcode >> 16) & 0xF800) == 0xE800 ? "blx" : "bl";
        line.Operands = Format("#0x%08x", line.Target);
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
            line.Mnemonic = insn[0].mnemonic;
            line.Operands = PlainRegisterNames(insn[0].op_str);
            cs_free(insn, n);
            return line;
        }
    }
#endif
    line.Mnemonic = thumb ? ".hword" : ".word";
    line.Operands = Format(thumb ? "0x%04x" : "0x%08x", line.Opcode);
    line.Kind = InstrKind::Data;
    return line;
}
