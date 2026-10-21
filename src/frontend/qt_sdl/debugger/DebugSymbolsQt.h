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

#include <QColor>
#include <QPalette>
#include <QString>

#include <string>

#include "DebugSource.h"
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

// Symbol files through DebugSource: local files, and http(s) downloads (github.com links become raw ones),
// cached on disk. readAll runs a nested event loop while downloading.
class QtSymbolFiles : public DebugSymbols::Files
{
public:
    bool isRemote(const std::string& location) const override { return DebugSource::IsRemote(SymStr(location)); }
    std::string normalize(const std::string& location) const override { return SymStr(DebugSource::Normalize(SymStr(location))); }
    std::string dirOf(const std::string& file) const override { return SymStr(DebugSource::DirOf(SymStr(file))); }
    std::string join(const std::string& dir, const std::string& relative) const override
    {
        return SymStr(DebugSource::Join(SymStr(dir), SymStr(relative)));
    }
    void readAll(const std::vector<std::string>& locations, std::unordered_map<std::string, std::string>& out) const override
    {
        QStringList wanted;
        for (const std::string& loc : locations)
            if (!loc.empty() && !out.count(loc))
                wanted << SymStr(loc);
        QHash<QString, QByteArray> files;
        DebugSource::ReadAll(wanted, files);
        for (auto it = files.begin(); it != files.end(); ++it)
            out[SymStr(it.key())] = it.value().toStdString();
    }
};

// Loads symbols from a path or URL, with the overlays of the running ROM (nds may be null).
inline bool LoadSymbols(DebugSymbols& syms, const QString& location, QString& error, melonDS::NDS* nds)
{
    static const QtSymbolFiles files;
    std::string err;
    bool ok = syms.load(SymStr(location), err, nds, &files);
    error = SymStr(err);
    return ok;
}

// Loads line info from an ELF at a path or URL (DebugSymbols::loadLines).
inline bool LoadLines(DebugSymbols& syms, const QString& location, QString& error)
{
    static const QtSymbolFiles files;
    std::string err;
    bool ok = syms.loadLines(SymStr(location), err, &files);
    error = SymStr(err);
    return ok;
}

// The colour of an instruction kind in the disassembly and the trace (DisasmLine::Kind).
inline QColor InstrColor(InstrKind kind, const QPalette& pal)
{
    bool dark = pal.base().color().lightness() < 128;
    switch (kind)
    {
    case InstrKind::Call: return dark ? QColor(0xF0, 0xA0, 0x50) : QColor(0xB0, 0x5A, 0x00);
    case InstrKind::Return: return dark ? QColor(0xF0, 0x78, 0x78) : QColor(0xC0, 0x28, 0x28);
    case InstrKind::Branch: return dark ? QColor(0xC8, 0xA0, 0xF8) : QColor(0x7B, 0x3F, 0xBF);
    case InstrKind::Load: return dark ? QColor(0x60, 0xC8, 0xE0) : QColor(0x00, 0x71, 0x8F);
    case InstrKind::Store: return dark ? QColor(0x88, 0xD0, 0x70) : QColor(0x2E, 0x7D, 0x32);
    case InstrKind::Compare: return dark ? QColor(0xE0, 0xD0, 0x60) : QColor(0x8A, 0x73, 0x00);
    case InstrKind::System: return dark ? QColor(0x78, 0xA8, 0xF8) : QColor(0x21, 0x55, 0xC4);
    case InstrKind::Data: return pal.placeholderText().color();
    default: return pal.text().color();
    }
}

#endif
