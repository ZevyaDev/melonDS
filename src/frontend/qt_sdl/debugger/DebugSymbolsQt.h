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

#ifndef DEBUGSYMBOLSQT_H
#define DEBUGSYMBOLSQT_H

#include <QString>

#include <string>

#include "DebugSymbols.h"
#include "Disassembler.h"

// The shared debug library is Qt-free and speaks UTF-8 std::string; these convert at the Qt boundary.
inline QString SymStr(const std::string& s)
{
    return QString::fromUtf8(s.data(), (int)s.size());
}

inline std::string SymStr(const QString& s)
{
    return s.toUtf8().toStdString();
}

#endif
