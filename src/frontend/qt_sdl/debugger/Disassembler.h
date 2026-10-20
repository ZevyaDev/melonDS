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

#ifndef DISASSEMBLER_H
#define DISASSEMBLER_H

#include <QColor>
#include <QString>

#include "types.h"

class QPalette;

enum class InstrKind : melonDS::u8
{
    Other,
    Call,
    Return,
    Branch,
    Load,
    Store,
    Compare,
    System,
    Data,
};

struct DisasmLine
{
    melonDS::u32 Addr;
    melonDS::u32 Opcode;
    melonDS::u8 Size;
    QString Mnemonic;
    QString Operands;
    melonDS::u32 Target;
    bool HasTarget;
    bool IsCall;
    bool IsLiteral;
    InstrKind Kind;
};

QColor InstrColor(InstrKind kind, const QPalette& pal);

class Disassembler
{
public:
    Disassembler();
    ~Disassembler();
    Disassembler(const Disassembler&) = delete;
    Disassembler& operator=(const Disassembler&) = delete;

    DisasmLine decode(melonDS::u32 addr, const melonDS::u8* bytes, int avail, bool thumb);
    static bool available();

private:
    void* Arm = nullptr;
    void* Thumb = nullptr;
};

#endif
