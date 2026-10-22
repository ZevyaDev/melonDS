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

#ifndef CALLARGS_H
#define CALLARGS_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <vector>

#include "debug/Debugger.h"

namespace melonDS { class NDS; }
class DebugSymbols;

namespace CallArgs
{

struct Memory
{
    QHash<melonDS::u32, melonDS::u32> Words;
    QHash<melonDS::u32, QByteArray> Strings;
};

struct Pointer
{
    QString Label;
    melonDS::u32 Addr;
};

struct Shown
{
    QString Text;
    QString Detail;
    QList<Pointer> Pointers;
};

void Collect(melonDS::NDS& nds, int cpu, const std::vector<melonDS::CallFrame>& frames, Memory& mem);
Shown Describe(const melonDS::CallFrame& frame, const DebugSymbols& syms, const Memory& mem);

}

#endif
