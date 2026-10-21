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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "types.h"
#include "debug/RomOverlays.h"

namespace melonDS
{
class NDS;
}

// A per-console symbol table. All strings are UTF-8. There is no global state:
// each owner (Qt DebugBridge, headless Instance) holds its own instance.
class DebugSymbols
{
public:
    struct Symbol
    {
        melonDS::u32 Addr;
        melonDS::u32 Size;
        std::string Name;
        std::string Pretty;
        std::string Kind;
        int Module;
        bool Thumb;
        std::string Original; // name before a label replaced it; still resolves
    };

    // A delinked source file's address range in a module (dsd delinks.txt): [Start, End).
    struct FileRange
    {
        melonDS::u32 Start;
        melonDS::u32 End;
        int File; // index into Set::SourceFiles
    };

    // A row of a DWARF line table (DwarfLines): from Addr on, the code is File:Line (File -1: no line).
    struct LineRow
    {
        melonDS::u32 Addr;
        int File; // index into Set::LineFiles
        int Line;
        bool Stmt;
    };

    struct Module
    {
        std::string Name;
        int Overlay = -1;
        melonDS::u32 Base = 0;
        melonDS::u32 Size = 0;
        std::vector<melonDS::u8> Image;
        std::vector<Symbol> Symbols;
        std::vector<FileRange> Files; // sorted by Start
        std::vector<LineRow> Lines;   // sorted by Addr

        const Symbol* find(melonDS::u32 addr) const;
        bool covers(melonDS::u32 addr) const { return addr >= Base && addr - Base < Size; }
    };

    struct Set
    {
        std::string Source;
        std::string Base;                     // the folder or URL the config's paths are relative to
        std::vector<Module> Modules;
        std::vector<std::string> SourceFiles; // delinked source paths, relative to Base
        // name -> (module, symbol index), in insertion order
        std::unordered_map<std::string, std::vector<std::pair<int, int>>> ByName;
        int SymbolCount = 0;
        std::string LineSource;             // the ELF the line info came from
        std::vector<std::string> LineFiles; // relative to the decomp root, like SourceFiles
        int LineCount = 0;
    };

    // Where symbol files are read from. The default reads local files only; the Qt frontend supplies one that
    // also downloads http(s) locations (a raw or github.com link to a config.yaml or its folder).
    class Files
    {
    public:
        virtual ~Files() = default;
        virtual bool isRemote(const std::string& location) const;
        // a folder becomes its config.yaml
        virtual std::string normalize(const std::string& location) const;
        virtual std::string dirOf(const std::string& file) const;
        virtual std::string join(const std::string& dir, const std::string& relative) const;
        // reads every location it can into out (keyed by location); missing ones are left out
        virtual void readAll(const std::vector<std::string>& locations, std::unordered_map<std::string, std::string>& out) const;
    };
    static const Files& localFiles();

    // ARM9 overlays as stored in a ROM, decompressed, by id (melonDS::ReadRomOverlays in the core).
    using RomOverlays = std::unordered_map<int, melonDS::RomOverlay>;
    // The overlays of the cart in nds (empty without a ROM). Reads the ROM: call it where the console is not
    // changing its cart (its emulator thread in headless).
    static RomOverlays readRomOverlays(melonDS::NDS* nds);

    // Loads a dsd config.yaml (or its folder) or an "address name [size]" text file. Overlay images come from
    // rom (the running ROM's overlays) when given and not empty, else from the extract the config's rom_config
    // names. The overload with nds reads the overlays from its cart. A local decomp config also loads the line info
    // of its build/<region>/arm9.o when that exists (loadLines; a failure there does not fail the load).
    bool load(const std::string& path, std::string& error, const RomOverlays* rom = nullptr, const Files* files = nullptr);
    bool load(const std::string& path, std::string& error, melonDS::NDS* nds, const Files* files = nullptr);
    // Line info: the DWARF .debug_line of an ELF linked with -sym on, matched to the modules through their
    // delinked source files. Needs symbols loaded first; kept until the next load or clear (labels keep it).
    bool loadLines(const std::string& path, std::string& error, const Files* files = nullptr);
    std::string lineSource() const { return Cur ? Cur->LineSource : std::string(); }
    // The file (relative, as in Set::LineFiles) and line of addr, among the static modules and loaded overlays.
    bool lineAt(melonDS::u32 addr, std::string& file, int& line) const;
    // The lowest statement address of file:line in a live module. module is the module that has the line
    // (also when false is returned because that overlay is not loaded), or -1.
    bool lineAddress(const std::string& file, int line, melonDS::u32& addr, int& module) const;
    void clear();
    bool empty() const { return !Cur || Cur->Modules.empty(); }
    std::string source() const { return Base ? Base->Source : std::string(); }

    // Labels: a text file of "address name [module]" lines applied on top of the loaded symbols.
    // A label renames the symbol starting at that address in that module (main, an autoload or
    // ovNNN; default: the static module covering the address) or adds one.
    bool loadLabels(const std::string& path, std::string& error);
    void clearLabels();
    const std::string& labelsSource() const { return LabelSource; }
    int labelCount() const { return (int)Labels.size(); }
    int labelsUnmatched() const { return Unmatched; }
    std::shared_ptr<const Set> set() const { return Cur; }

    // Which overlays of the set are currently resident, judged by sampling memory through peek.
    static std::vector<int> detect(const Set& set, const std::function<melonDS::u32(melonDS::u32)>& peek);
    static std::vector<int> detect(const Set& set, melonDS::NDS& nds);
    void setLoaded(const std::vector<int>& overlays) { Loaded = overlays; }
    const std::vector<int>& loaded() const { return Loaded; }

    void setDemangle(bool on) { Demangle = on; }
    bool demangle() const { return Demangle; }
    const std::string& name(const Symbol& sym) const { return Demangle ? sym.Pretty : sym.Name; }

    const Symbol* lookup(melonDS::u32 addr) const;
    const Module* moduleOf(const Symbol* sym) const;
    // The delinked source file (relative to set()->Base) that holds addr, from the config's delinks.txt.
    bool sourceFile(melonDS::u32 addr, std::string& file) const;
    // An overlay of the set has no image to detect it by (the ROM was not available at load).
    bool needsRom() const;
    std::string describe(melonDS::u32 addr, bool withModule = true) const;
    std::string exact(melonDS::u32 addr) const;
    bool resolve(const std::string& name, melonDS::u32& out) const;
    std::vector<const Symbol*> search(const std::string& text, int limit) const;
    bool parseAddress(const std::string& text, melonDS::u32& out) const;

private:
    struct Label
    {
        melonDS::u32 Addr;
        std::string Name;
        std::string Module;
    };

    void rebuild();

    std::shared_ptr<const Set> Base; // as loaded
    std::shared_ptr<const Set> Cur;  // Base with Labels applied
    std::vector<Label> Labels;
    std::string LabelSource;
    int Unmatched = 0;
    std::vector<int> Loaded;
    bool Loading = false; // a Files reader may run an event loop; refuse to re-enter load
    bool Demangle = true;
};

#endif
