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

#include <algorithm>
#include <string>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include "DebugSource.h"
#include "DebugSymbols.h"
#include "NDS.h"
#include "debug/DwarfLines.h"
#include "debug/RomOverlays.h"

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define HAVE_CXXABI
#endif

using namespace melonDS;

namespace
{

QString DemangleName(const QString& name)
{
#ifdef HAVE_CXXABI
    if (!name.startsWith("_Z"))
        return name;
    int status = 0;
    char* out = abi::__cxa_demangle(name.toLatin1().constData(), nullptr, nullptr, &status);
    if (status != 0 || !out)
        return name;
    QString pretty = QString::fromLatin1(out);
    free(out);
    return pretty;
#else
    return name;
#endif
}

struct YNode
{
    std::string Scalar;
    std::vector<std::pair<std::string, YNode>> Map;
    std::vector<YNode> List;

    const YNode* get(const std::string& key) const
    {
        for (auto& [k, v] : Map)
            if (k == key)
                return &v;
        return nullptr;
    }

    std::string str(const std::string& key) const
    {
        const YNode* n = get(key);
        return n ? n->Scalar : std::string();
    }

    u32 num(const std::string& key) const
    {
        const YNode* n = get(key);
        if (!n || n->Scalar.empty())
            return 0;
        return (u32)std::stoull(n->Scalar, nullptr, 0);
    }
};

struct YLine
{
    int Indent;
    std::string Text;
};

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string Unquote(const std::string& raw)
{
    std::string s = Trim(raw);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
    {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); i++)
        {
            out += s[i];
            if (s[i] == '\'' && s[i + 1] == '\'')
                i++;
        }
        return out;
    }
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    if (s == "[]" || s == "{}" || s == "~" || s == "null")
        return "";
    return s;
}

bool IsDash(const std::string& t)
{
    return t == "-" || (t.size() >= 2 && t[0] == '-' && t[1] == ' ');
}

size_t KeySplit(const std::string& t)
{
    char quote = 0;
    for (size_t i = 0; i < t.size(); i++)
    {
        char c = t[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"')
            quote = c;
        else if (c == ':' && (i + 1 == t.size() || t[i + 1] == ' '))
            return i;
    }
    return std::string::npos;
}

YNode ParseBlock(std::vector<YLine>& lines, size_t& i, int indent);

YNode ParseMap(std::vector<YLine>& lines, size_t& i, int indent)
{
    YNode node;
    while (i < lines.size())
    {
        YLine& line = lines[i];
        if (line.Indent < indent || (line.Indent == indent && IsDash(line.Text)))
            break;
        if (line.Indent > indent)
        {
            i++;
            continue;
        }
        size_t colon = KeySplit(line.Text);
        if (colon == std::string::npos)
        {
            i++;
            continue;
        }
        std::string key = Unquote(line.Text.substr(0, colon));
        std::string rest = Trim(line.Text.substr(colon + 1));
        i++;
        YNode child;
        if (!rest.empty())
            child.Scalar = Unquote(rest);
        else if (i < lines.size() && (lines[i].Indent > indent || (lines[i].Indent == indent && IsDash(lines[i].Text))))
            child = ParseBlock(lines, i, lines[i].Indent);
        node.Map.emplace_back(std::move(key), std::move(child));
    }
    return node;
}

YNode ParseList(std::vector<YLine>& lines, size_t& i, int indent)
{
    YNode node;
    while (i < lines.size() && lines[i].Indent == indent && IsDash(lines[i].Text))
    {
        std::string rest = Trim(lines[i].Text.substr(1));
        if (rest.empty())
        {
            i++;
            if (i < lines.size() && lines[i].Indent > indent)
                node.List.push_back(ParseBlock(lines, i, lines[i].Indent));
            else
                node.List.emplace_back();
            continue;
        }
        if (KeySplit(rest) != std::string::npos)
        {
            lines[i].Indent = indent + 2;
            lines[i].Text = rest;
            node.List.push_back(ParseMap(lines, i, indent + 2));
            continue;
        }
        YNode item;
        item.Scalar = Unquote(rest);
        node.List.push_back(std::move(item));
        i++;
    }
    return node;
}

YNode ParseBlock(std::vector<YLine>& lines, size_t& i, int indent)
{
    if (i < lines.size() && IsDash(lines[i].Text))
        return ParseList(lines, i, indent);
    return ParseMap(lines, i, indent);
}

bool ParseYAML(const QByteArray& data, YNode& out)
{
    if (data.isEmpty())
        return false;
    std::vector<YLine> lines;
    for (const QByteArray& raw : data.split('\n'))
    {
        std::string s = raw.toStdString();
        std::string t = Trim(s);
        if (t.empty() || t[0] == '#' || t == "---")
            continue;
        int indent = (int)s.find_first_not_of(' ');
        lines.push_back({indent, t});
    }
    size_t i = 0;
    out = ParseBlock(lines, i, lines.empty() ? 0 : lines[0].Indent);
    return true;
}

u32 ParseNumber(const std::string& s, bool& ok)
{
    try
    {
        size_t used;
        unsigned long long v = std::stoull(s, &used, 0);
        ok = used == s.size();
        return (u32)v;
    }
    catch (...)
    {
        ok = false;
        return 0;
    }
}

void ParseDsdSymbols(const QByteArray& data, int module, std::vector<DebugSymbols::Symbol>& out)
{
    for (const QByteArray& raw : data.split('\n'))
    {
        std::string line = Trim(raw.toStdString());
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos)
            continue;
        size_t kindPos = line.find("kind:", sp);
        size_t addrPos = line.find("addr:0x", sp);
        if (kindPos == std::string::npos || addrPos == std::string::npos)
            continue;
        DebugSymbols::Symbol sym {};
        sym.Name = QString::fromStdString(line.substr(0, sp));
        sym.Module = module;
        size_t kindEnd = kindPos + 5;
        while (kindEnd < line.size() && (isalnum((unsigned char)line[kindEnd]) || line[kindEnd] == '_'))
            kindEnd++;
        sym.Kind = QString::fromStdString(line.substr(kindPos + 5, kindEnd - kindPos - 5));
        if (kindEnd < line.size() && line[kindEnd] == '(')
        {
            size_t close = line.find(')', kindEnd);
            std::string args = line.substr(kindEnd + 1, close == std::string::npos ? std::string::npos : close - kindEnd - 1);
            size_t start = 0;
            while (start <= args.size())
            {
                size_t comma = args.find(',', start);
                std::string part = Trim(args.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                bool ok;
                if (part.rfind("size=", 0) == 0)
                    sym.Size = ParseNumber(part.substr(5), ok);
                else if (part == "thumb")
                    sym.Thumb = true;
                else if (part == "byte" || part == "short" || part == "word")
                    sym.Size = part == "byte" ? 1 : part == "short" ? 2 : 4;
                else if (part.rfind("byte[", 0) == 0 || part.rfind("short[", 0) == 0 || part.rfind("word[", 0) == 0)
                {
                    size_t lb = part.find('['), rb = part.find(']');
                    std::string count = part.substr(lb + 1, rb - lb - 1);
                    u32 n = count.empty() ? 1 : ParseNumber(count, ok);
                    sym.Size = n * (part[0] == 'b' ? 1 : part[0] == 's' ? 2 : 4);
                }
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        }
        bool ok;
        size_t addrEnd = line.find_first_of(" \t", addrPos);
        sym.Addr = ParseNumber(line.substr(addrPos + 5, addrEnd == std::string::npos ? std::string::npos : addrEnd - addrPos - 5), ok);
        if (ok)
            out.push_back(std::move(sym));
    }
}

std::string Field(const std::string& line, const std::string& key)
{
    size_t at = line.find(key);
    if (at == std::string::npos)
        return std::string();
    at += key.size();
    size_t end = line.find_first_of(" \t", at);
    return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void ParseDelinks(const QByteArray& data, DebugSymbols::Set& set, QHash<QString, int>& index, std::vector<DebugSymbols::FileRange>& out)
{
    int file = -1;
    for (const QByteArray& raw : data.split('\n'))
    {
        std::string line = raw.toStdString();
        std::string t = Trim(line);
        if (t.empty() || t.rfind("//", 0) == 0)
            continue;
        if (line[0] != ' ' && line[0] != '\t')
        {
            file = -1;
            if (t.back() != ':')
                continue;
            QString path = QString::fromStdString(t.substr(0, t.size() - 1));
            auto it = index.find(path);
            if (it == index.end())
            {
                it = index.insert(path, (int)set.SourceFiles.size());
                set.SourceFiles << path;
            }
            file = *it;
            continue;
        }
        if (file < 0)
            continue;
        bool okStart, okEnd;
        u32 start = ParseNumber(Field(t, "start:"), okStart);
        u32 end = ParseNumber(Field(t, "end:"), okEnd);
        if (okStart && okEnd && end > start)
            out.push_back({start, end, file});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.Start < b.Start; });
}

QString Str(const YNode& node, const std::string& key)
{
    return QString::fromStdString(node.str(key));
}

void Index(DebugSymbols::Module& mod)
{
    std::sort(mod.Symbols.begin(), mod.Symbols.end(), [](const auto& a, const auto& b) { return a.Addr < b.Addr; });
    if (!mod.Size && !mod.Symbols.empty())
    {
        auto& last = mod.Symbols.back();
        if (!mod.Base)
            mod.Base = mod.Symbols.front().Addr;
        mod.Size = last.Addr + std::max<u32>(last.Size, 1) - mod.Base;
    }
}

}

const DebugSymbols::Symbol* DebugSymbols::Module::find(u32 addr) const
{
    auto it = std::upper_bound(Symbols.begin(), Symbols.end(), addr, [](u32 a, const Symbol& s) { return a < s.Addr; });
    if (it == Symbols.begin())
        return nullptr;
    --it;
    if (addr < it->Addr + std::max<u32>(it->Size, 1))
        return &*it;
    return nullptr;
}

bool DebugSymbols::load(const QString& rawPath, QString& error, NDS* nds)
{
    static bool busy = false;
    if (busy)
    {
        error = "symbols are already loading";
        return false;
    }
    busy = true;
    struct Release
    {
        ~Release() { busy = false; }
    } release;

    QString path = DebugSource::Normalize(rawPath);
    bool remote = DebugSource::IsRemote(path);
    QHash<QString, QByteArray> files;
    DebugSource::ReadAll({path}, files);
    if (!files.contains(path))
    {
        error = (remote ? "cannot download " : "not found: ") + path;
        return false;
    }

    auto set = std::make_shared<Set>();
    set->Source = path;
    set->Base = DebugSource::DirOf(path);
    QFileInfo info(remote ? QUrl(path).path() : path);

    if (info.suffix() == "yaml" || info.suffix() == "yml")
    {
        YNode cfg;
        if (!ParseYAML(files.value(path), cfg))
        {
            error = "cannot read " + path;
            return false;
        }
        const YNode* main = cfg.get("main_module");
        if (!main)
        {
            error = "no main_module in " + path;
            return false;
        }
        struct Entry
        {
            QString Name;
            int Overlay;
            QString Symbols;
            QString Delinks;
        };
        std::vector<Entry> entries;
        auto entry = [&](const QString& name, int overlay, const YNode& node) {
            QString symbols = Str(node, "symbols"), delinks = Str(node, "delinks");
            entries.push_back({name, overlay,
                symbols.isEmpty() ? QString() : DebugSource::Join(set->Base, symbols),
                delinks.isEmpty() ? QString() : DebugSource::Join(set->Base, delinks)});
        };
        entry("main", -1, *main);
        if (const YNode* autoloads = cfg.get("autoloads"))
            for (const YNode& a : autoloads->List)
                entry(Str(a, "name"), -1, a);
        if (const YNode* overlays = cfg.get("overlays"))
            for (const YNode& ov : overlays->List)
                entry(Str(ov, "name"), (int)ov.num("id"), ov);
        QStringList wanted;
        for (const Entry& e : entries)
            wanted << e.Symbols << e.Delinks;
        DebugSource::ReadAll(wanted, files);

        QHash<int, RomOverlay> table;
        if (nds)
            for (RomOverlay& ov : ReadRomOverlays(*nds))
                table.insert(ov.Id, std::move(ov));
        std::string romCfg = cfg.str("rom_config");
        if (table.isEmpty() && !remote && !romCfg.empty())
        {
            QString romDir = DebugSource::DirOf(DebugSource::Join(set->Base, QString::fromStdString(romCfg)));
            QFile tableFile(DebugSource::Join(romDir, "arm9_overlays/overlays.yaml"));
            YNode ovTable;
            if (tableFile.open(QIODevice::ReadOnly) && ParseYAML(tableFile.readAll(), ovTable))
                if (const YNode* list = ovTable.get("overlays"))
                    for (const YNode& ov : list->List)
                    {
                        QFile file(DebugSource::Join(romDir, "arm9_overlays/" + Str(ov, "file_name")));
                        QByteArray image = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
                        int id = (int)ov.num("id");
                        table.insert(id, {id, ov.num("base_address"), ov.num("code_size") + ov.num("bss_size"),
                            std::vector<u8>(image.begin(), image.end())});
                    }
        }

        QHash<QString, int> fileIndex;
        for (const Entry& e : entries)
        {
            Module mod;
            mod.Name = e.Name;
            mod.Overlay = e.Overlay;
            ParseDsdSymbols(files.value(e.Symbols), (int)set->Modules.size(), mod.Symbols);
            ParseDelinks(files.value(e.Delinks), *set, fileIndex, mod.Files);
            auto it = table.find(e.Overlay);
            if (e.Overlay >= 0 && it != table.end())
            {
                mod.Base = it->Base;
                mod.Size = it->Size;
                mod.Image = it->Image;
            }
            set->Modules.push_back(std::move(mod));
        }
    }
    else
    {
        Module mod;
        mod.Name = info.baseName();
        for (const QByteArray& raw : files.value(path).split('\n'))
        {
            QList<QByteArray> parts = raw.simplified().split(' ');
            if (parts.size() < 2)
                continue;
            bool ok;
            u32 addr = parts[0].toUInt(&ok, 16);
            if (!ok)
                continue;
            Symbol sym {};
            sym.Addr = addr & ~1u;
            sym.Thumb = addr & 1;
            sym.Name = QString::fromUtf8(parts[1]);
            sym.Kind = "function";
            sym.Size = parts.size() > 2 ? parts[2].toUInt(nullptr, 0) : 0;
            mod.Symbols.push_back(std::move(sym));
        }
        set->Modules.push_back(std::move(mod));
    }

    for (int m = 0; m < (int)set->Modules.size(); m++)
    {
        Module& mod = set->Modules[m];
        Index(mod);
        for (int s = 0; s < (int)mod.Symbols.size(); s++)
        {
            Symbol& sym = mod.Symbols[s];
            sym.Pretty = DemangleName(sym.Name);
            set->ByName.insert(sym.Name, {m, s});
            if (sym.Pretty != sym.Name)
            {
                set->ByName.insert(sym.Pretty, {m, s});
                int paren = sym.Pretty.indexOf('(');
                if (paren > 0)
                    set->ByName.insert(sym.Pretty.left(paren), {m, s});
            }
        }
        set->SymbolCount += (int)mod.Symbols.size();
    }
    if (!set->SymbolCount)
    {
        error = "no symbols in " + path;
        return false;
    }
    Cur = set;
    Loaded.clear();
    if (!remote && (info.suffix() == "yaml" || info.suffix() == "yml"))
    {
        QDir dir(set->Base);
        dir.cdUp();
        QString region = dir.dirName();
        QString elf = dir.cdUp() && dir.cdUp() ? dir.filePath("build/" + region + "/arm9.o") : QString();
        QString ignored;
        if (!elf.isEmpty() && QFile::exists(elf))
            loadLines(elf, ignored);
    }
    return true;
}

bool DebugSymbols::loadLines(const QString& rawPath, QString& error)
{
    if (!Cur)
    {
        error = "load symbols first";
        return false;
    }
    QString path = DebugSource::Normalize(rawPath);
    QHash<QString, QByteArray> files;
    DebugSource::ReadAll({path}, files);
    if (!files.contains(path))
    {
        error = (DebugSource::IsRemote(path) ? "cannot download " : "not found: ") + path;
        return false;
    }
    const QByteArray& elf = files[path];
    std::vector<DwarfLineProgram> programs;
    std::string why;
    if (!ReadDwarfLines((const u8*)elf.constData(), (size_t)elf.size(), programs, why))
    {
        error = QString::fromStdString(why) + ": " + path;
        return false;
    }

    auto set = std::make_shared<Set>(*Cur);
    set->LineSource = path;
    set->LineFiles.clear();
    set->LineCount = 0;
    QHash<QString, int> moduleOf;
    for (int m = 0; m < (int)set->Modules.size(); m++)
    {
        set->Modules[m].Lines.clear();
        for (const FileRange& r : set->Modules[m].Files)
            moduleOf.insert(set->SourceFiles[r.File].toLower(), m);
    }
    QHash<QString, int> fileIndex;
    for (const DwarfLineProgram& prog : programs)
    {
        std::vector<QString> paths;
        for (const std::string& f : prog.Files)
            paths.push_back(QString::fromStdString(f).replace('\\', '/'));
        int module = -1;
        QString root;
        for (size_t f = 1; f < paths.size() && module < 0; f++)
        {
            QStringList parts = paths[f].split('/');
            for (int i = 0; i < parts.size() && module < 0; i++)
            {
                QString rel = parts.mid(i).join('/');
                auto it = moduleOf.constFind(rel.toLower());
                if (it == moduleOf.constEnd())
                    continue;
                module = *it;
                root = paths[f].left(paths[f].size() - rel.size());
            }
        }
        if (module < 0)
            continue;
        std::vector<int> index(paths.size(), -1);
        for (size_t f = 1; f < paths.size(); f++)
        {
            QString rel = paths[f].startsWith(root, Qt::CaseInsensitive) ? paths[f].mid(root.size()) : paths[f];
            auto it = fileIndex.constFind(rel);
            if (it == fileIndex.constEnd())
            {
                it = fileIndex.insert(rel, (int)set->LineFiles.size());
                set->LineFiles << rel;
            }
            index[f] = *it;
        }
        std::vector<LineRow>& lines = set->Modules[module].Lines;
        for (const DwarfLineRow& row : prog.Rows)
        {
            int file = row.End || row.File >= index.size() ? -1 : index[row.File];
            lines.push_back({row.Addr, file, (int)row.Line, row.Stmt});
            set->LineCount += file >= 0;
        }
    }
    if (!set->LineCount)
    {
        error = "no line info for the source files of these symbols: " + path;
        return false;
    }
    for (Module& mod : set->Modules)
        std::stable_sort(mod.Lines.begin(), mod.Lines.end(), [](const LineRow& a, const LineRow& b) {
            return a.Addr != b.Addr ? a.Addr < b.Addr : (a.File < 0 && b.File >= 0);
        });
    Cur = set;
    return true;
}

bool DebugSymbols::lineAt(u32 addr, QString& file, int& line) const
{
    if (!Cur)
        return false;
    addr &= ~1u;
    for (const Module& mod : Cur->Modules)
    {
        if (mod.Lines.empty() || (mod.Overlay >= 0 && std::find(Loaded.begin(), Loaded.end(), mod.Overlay) == Loaded.end()))
            continue;
        auto it = std::upper_bound(mod.Lines.begin(), mod.Lines.end(), addr, [](u32 a, const LineRow& r) { return a < r.Addr; });
        if (it == mod.Lines.begin() || it == mod.Lines.end())
            continue;
        --it;
        if (it->File < 0)
            continue;
        file = Cur->LineFiles[it->File];
        line = it->Line;
        return true;
    }
    return false;
}

bool DebugSymbols::lineAddress(const QString& file, int line, u32& addr, int& module) const
{
    module = -1;
    if (!Cur)
        return false;
    int f = Cur->LineFiles.indexOf(file);
    if (f < 0)
        return false;
    bool found = false;
    for (int m = 0; m < (int)Cur->Modules.size(); m++)
    {
        const Module& mod = Cur->Modules[m];
        bool live = mod.Overlay < 0 || std::find(Loaded.begin(), Loaded.end(), mod.Overlay) != Loaded.end();
        for (const LineRow& r : mod.Lines)
        {
            if (r.File != f || r.Line != line || !r.Stmt)
                continue;
            if (!live)
            {
                if (module < 0)
                    module = m;
                break;
            }
            if (!found || r.Addr < addr)
                addr = r.Addr;
            found = true;
            module = m;
        }
        if (found)
            return true;
    }
    return false;
}

void DebugSymbols::clear()
{
    Cur.reset();
    Loaded.clear();
}

std::vector<int> DebugSymbols::detect(const Set& set, NDS& nds)
{
    std::vector<int> out;
    for (const Module& mod : set.Modules)
        if (mod.Overlay >= 0 && OverlayLoaded(nds, mod.Base, mod.Size, mod.Image))
            out.push_back(mod.Overlay);
    return out;
}

const DebugSymbols::Symbol* DebugSymbols::lookup(u32 addr) const
{
    if (!Cur)
        return nullptr;
    addr &= ~1u;
    for (const Module& mod : Cur->Modules)
        if (mod.Overlay < 0)
            if (const Symbol* s = mod.find(addr))
                return s;
    for (const Module& mod : Cur->Modules)
        if (mod.Overlay >= 0 && std::find(Loaded.begin(), Loaded.end(), mod.Overlay) != Loaded.end())
            if (const Symbol* s = mod.find(addr))
                return s;
    return nullptr;
}

const DebugSymbols::Module* DebugSymbols::moduleOf(const Symbol* sym) const
{
    if (!Cur || !sym || sym->Module < 0 || sym->Module >= (int)Cur->Modules.size())
        return nullptr;
    return &Cur->Modules[sym->Module];
}

bool DebugSymbols::needsRom() const
{
    if (!Cur)
        return false;
    for (const Module& mod : Cur->Modules)
        if (mod.Overlay >= 0 && mod.Image.empty())
            return true;
    return false;
}

bool DebugSymbols::sourceFile(u32 addr, QString& file) const
{
    if (!Cur)
        return false;
    addr &= ~1u;
    auto search = [&](const Module& mod) {
        auto it = std::upper_bound(mod.Files.begin(), mod.Files.end(), addr, [](u32 a, const FileRange& r) { return a < r.Start; });
        if (it == mod.Files.begin() || addr >= (--it)->End)
            return false;
        file = Cur->SourceFiles[it->File];
        return true;
    };
    if (const Module* mod = moduleOf(lookup(addr)))
        if (search(*mod))
            return true;
    for (const Module& mod : Cur->Modules)
        if (mod.Overlay < 0 || std::find(Loaded.begin(), Loaded.end(), mod.Overlay) != Loaded.end())
            if (search(mod))
                return true;
    return false;
}

QString DebugSymbols::describe(u32 addr, bool withModule) const
{
    const Symbol* s = lookup(addr);
    if (!s)
        return QString();
    u32 off = (addr & ~1u) - s->Addr;
    QString out = off ? QString("%1+0x%2").arg(name(*s)).arg(off, 0, 16) : name(*s);
    const Module* mod = moduleOf(s);
    if (withModule && mod && mod->Overlay >= 0)
        out += " [" + mod->Name + "]";
    return out;
}

QString DebugSymbols::exact(u32 addr) const
{
    const Symbol* s = lookup(addr);
    if (s && s->Addr == (addr & ~1u))
        return name(*s);
    return QString();
}

bool DebugSymbols::resolve(const QString& name, u32& out) const
{
    if (!Cur)
        return false;
    QList<std::pair<int, int>> hits = Cur->ByName.values(name);
    if (hits.isEmpty())
        return false;
    for (auto& [m, s] : hits)
    {
        const Module& mod = Cur->Modules[m];
        if (mod.Overlay < 0 || std::find(Loaded.begin(), Loaded.end(), mod.Overlay) != Loaded.end())
        {
            out = mod.Symbols[s].Addr;
            return true;
        }
    }
    out = Cur->Modules[hits.front().first].Symbols[hits.front().second].Addr;
    return true;
}

std::vector<const DebugSymbols::Symbol*> DebugSymbols::search(const QString& text, int limit) const
{
    std::vector<const Symbol*> out;
    if (!Cur)
        return out;
    for (const Module& mod : Cur->Modules)
        for (const Symbol& s : mod.Symbols)
            if (s.Name.contains(text, Qt::CaseInsensitive) || s.Pretty.contains(text, Qt::CaseInsensitive))
            {
                out.push_back(&s);
                if ((int)out.size() >= limit)
                    return out;
            }
    return out;
}

bool DebugSymbols::parseAddress(const QString& raw, u32& out) const
{
    QString text = raw.trimmed();
    if (text.isEmpty())
        return false;
    int plus = text.lastIndexOf('+');
    u32 offset = 0;
    if (plus > 0)
    {
        bool ok;
        QString off = text.mid(plus + 1).trimmed();
        offset = off.startsWith("0x", Qt::CaseInsensitive) ? off.mid(2).toUInt(&ok, 16) : off.toUInt(&ok, 16);
        if (ok)
            text = text.left(plus).trimmed();
        else
            offset = 0;
    }
    if (resolve(text, out))
    {
        out += offset;
        return true;
    }
    QString hex = text.startsWith("0x", Qt::CaseInsensitive) ? text.mid(2) : text;
    bool ok;
    out = hex.toUInt(&ok, 16) + offset;
    return ok;
}
