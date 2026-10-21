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

#ifndef DEBUGSYMBOLS_H
#define DEBUGSYMBOLS_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "types.h"

namespace melonDS
{
class NDS;
}

class DebugSymbols
{
public:
    struct Symbol
    {
        melonDS::u32 Addr;
        melonDS::u32 Size;
        QString Name;
        QString Pretty;
        QString Kind;
        int Module;
        bool Thumb;
    };

    struct FileRange
    {
        melonDS::u32 Start;
        melonDS::u32 End;
        int File;
    };

    struct LineRow
    {
        melonDS::u32 Addr;
        int File;
        int Line;
        bool Stmt;
    };

    struct Module
    {
        QString Name;
        int Overlay = -1;
        melonDS::u32 Base = 0;
        melonDS::u32 Size = 0;
        std::vector<melonDS::u8> Image;
        std::vector<Symbol> Symbols;
        std::vector<FileRange> Files;
        std::vector<LineRow> Lines;

        const Symbol* find(melonDS::u32 addr) const;
        bool covers(melonDS::u32 addr) const { return addr >= Base && addr - Base < Size; }
    };

    struct Set
    {
        QString Source;
        QString Base;
        std::vector<Module> Modules;
        QStringList SourceFiles;
        QMultiHash<QString, std::pair<int, int>> ByName;
        int SymbolCount = 0;
        QString LineSource;
        QStringList LineFiles;
        int LineCount = 0;
    };

    bool load(const QString& path, QString& error, melonDS::NDS* nds = nullptr);
    bool loadLines(const QString& path, QString& error);
    QString lineSource() const { return Cur ? Cur->LineSource : QString(); }
    bool lineAt(melonDS::u32 addr, QString& file, int& line) const;
    bool lineAddress(const QString& file, int line, melonDS::u32& addr, int& module) const;
    void clear();
    bool empty() const { return !Cur || Cur->Modules.empty(); }
    QString source() const { return Cur ? Cur->Source : QString(); }
    std::shared_ptr<const Set> set() const { return Cur; }

    static std::vector<int> detect(const Set& set, melonDS::NDS& nds);
    void setLoaded(const std::vector<int>& overlays) { Loaded = overlays; }
    const std::vector<int>& loaded() const { return Loaded; }

    void setDemangle(bool on) { Demangle = on; }
    bool demangle() const { return Demangle; }
    const QString& name(const Symbol& sym) const { return Demangle ? sym.Pretty : sym.Name; }

    const Symbol* lookup(melonDS::u32 addr) const;
    const Module* moduleOf(const Symbol* sym) const;
    bool sourceFile(melonDS::u32 addr, QString& file) const;
    bool needsRom() const;
    QString describe(melonDS::u32 addr, bool withModule = true) const;
    QString exact(melonDS::u32 addr) const;
    bool resolve(const QString& name, melonDS::u32& out) const;
    std::vector<const Symbol*> search(const QString& text, int limit) const;
    bool parseAddress(const QString& text, melonDS::u32& out) const;

private:
    std::shared_ptr<const Set> Cur;
    std::vector<int> Loaded;
    bool Demangle = true;
};

#endif
