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
        QString Original; // name before a label replaced it; still resolves
    };

    struct Module
    {
        QString Name;
        int Overlay = -1;
        melonDS::u32 Base = 0;
        melonDS::u32 Size = 0;
        QByteArray Image;
        std::vector<Symbol> Symbols;

        const Symbol* find(melonDS::u32 addr) const;
        bool covers(melonDS::u32 addr) const { return addr >= Base && addr - Base < Size; }
    };

    struct Set
    {
        QString Source;
        std::vector<Module> Modules;
        QMultiHash<QString, std::pair<int, int>> ByName;
        int SymbolCount = 0;
    };

    bool load(const QString& path, QString& error);
    void clear();
    bool empty() const { return !Cur || Cur->Modules.empty(); }
    QString source() const { return Base ? Base->Source : QString(); }

    // Labels: a text file of "address name [module]" lines applied on top of the loaded symbols.
    // A label renames the symbol starting at that address in that module (main, an autoload or
    // ovNNN; default: the static module covering the address) or adds one.
    bool loadLabels(const QString& path, QString& error);
    void clearLabels();
    QString labelsSource() const { return LabelSource; }
    int labelCount() const { return (int)Labels.size(); }
    int labelsUnmatched() const { return Unmatched; }
    std::shared_ptr<const Set> set() const { return Cur; }

    static std::vector<int> detect(const Set& set, melonDS::NDS& nds);
    void setLoaded(const std::vector<int>& overlays) { Loaded = overlays; }
    const std::vector<int>& loaded() const { return Loaded; }

    void setDemangle(bool on) { Demangle = on; }
    bool demangle() const { return Demangle; }
    const QString& name(const Symbol& sym) const { return Demangle ? sym.Pretty : sym.Name; }

    const Symbol* lookup(melonDS::u32 addr) const;
    const Module* moduleOf(const Symbol* sym) const;
    QString describe(melonDS::u32 addr, bool withModule = true) const;
    QString exact(melonDS::u32 addr) const;
    bool resolve(const QString& name, melonDS::u32& out) const;
    std::vector<const Symbol*> search(const QString& text, int limit) const;
    bool parseAddress(const QString& text, melonDS::u32& out) const;

private:
    struct Label
    {
        melonDS::u32 Addr;
        QString Name;
        QString Module;
    };

    void rebuild();

    std::shared_ptr<const Set> Base; // as loaded
    std::shared_ptr<const Set> Cur;  // Base with Labels applied
    std::vector<Label> Labels;
    QString LabelSource;
    int Unmatched = 0;
    std::vector<int> Loaded;
    bool Demangle = true;
};

#endif
