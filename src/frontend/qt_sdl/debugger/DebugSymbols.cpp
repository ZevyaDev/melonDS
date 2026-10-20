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

#include "DebugSymbols.h"
#include "NDS.h"

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

constexpr int SampleWindows = 8;
constexpr int SampleBytes = 16;

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

bool ParseYAML(const QString& path, YNode& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    std::vector<YLine> lines;
    for (const QByteArray& raw : f.readAll().split('\n'))
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

void ParseDsdSymbols(const QString& path, int module, std::vector<DebugSymbols::Symbol>& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    for (const QByteArray& raw : f.readAll().split('\n'))
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

// Sort every module and rebuild the name index; run after the symbols change.
void Finalize(DebugSymbols::Set& set)
{
    set.ByName.clear();
    set.SymbolCount = 0;
    for (int m = 0; m < (int)set.Modules.size(); m++)
    {
        DebugSymbols::Module& mod = set.Modules[m];
        Index(mod);
        for (int s = 0; s < (int)mod.Symbols.size(); s++)
        {
            DebugSymbols::Symbol& sym = mod.Symbols[s];
            sym.Pretty = DemangleName(sym.Name);
            set.ByName.insert(sym.Name, {m, s});
            if (sym.Pretty != sym.Name)
            {
                set.ByName.insert(sym.Pretty, {m, s});
                int paren = sym.Pretty.indexOf('(');
                if (paren > 0)
                    set.ByName.insert(sym.Pretty.left(paren), {m, s});
            }
            if (!sym.Original.isEmpty())
                set.ByName.insert(sym.Original, {m, s});
        }
        set.SymbolCount += (int)mod.Symbols.size();
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

bool DebugSymbols::load(const QString& rawPath, QString& error)
{
    QString path = rawPath;
    QFileInfo info(path);
    if (info.isDir())
        path = QDir(path).filePath("config.yaml");
    info = QFileInfo(path);
    if (!info.exists())
    {
        error = "not found: " + path;
        return false;
    }

    auto set = std::make_shared<Set>();
    set->Source = path;

    if (info.suffix() == "yaml" || info.suffix() == "yml")
    {
        YNode cfg;
        if (!ParseYAML(path, cfg))
        {
            error = "cannot read " + path;
            return false;
        }
        QDir root = info.dir();
        auto add = [&](const QString& name, int overlay, const std::string& symbols) {
            Module mod;
            mod.Name = name;
            mod.Overlay = overlay;
            ParseDsdSymbols(root.filePath(QString::fromStdString(symbols)), (int)set->Modules.size(), mod.Symbols);
            set->Modules.push_back(std::move(mod));
        };
        const YNode* main = cfg.get("main_module");
        if (!main)
        {
            error = "no main_module in " + path;
            return false;
        }
        add("main", -1, main->str("symbols"));
        if (const YNode* autoloads = cfg.get("autoloads"))
            for (const YNode& a : autoloads->List)
                add(QString::fromStdString(a.str("name")), -1, a.str("symbols"));

        QHash<int, std::pair<YNode, QString>> table;
        std::string romCfg = cfg.str("rom_config");
        if (!romCfg.empty())
        {
            QDir romDir = QFileInfo(root.filePath(QString::fromStdString(romCfg))).dir();
            QString tablePath = romDir.filePath("arm9_overlays/overlays.yaml");
            YNode ovTable;
            if (ParseYAML(tablePath, ovTable))
                if (const YNode* list = ovTable.get("overlays"))
                    for (const YNode& ov : list->List)
                        table.insert((int)ov.num("id"), {ov, romDir.filePath("arm9_overlays/" + QString::fromStdString(ov.str("file_name")))});
        }
        if (const YNode* overlays = cfg.get("overlays"))
            for (const YNode& ov : overlays->List)
            {
                int id = (int)ov.num("id");
                add(QString::fromStdString(ov.str("name")), id, ov.str("symbols"));
                Module& mod = set->Modules.back();
                auto it = table.find(id);
                if (it != table.end())
                {
                    mod.Base = it->first.num("base_address");
                    mod.Size = it->first.num("code_size") + it->first.num("bss_size");
                    QFile image(it->second);
                    if (image.open(QIODevice::ReadOnly))
                        mod.Image = image.readAll();
                }
            }
    }
    else
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
        {
            error = "cannot read " + path;
            return false;
        }
        Module mod;
        mod.Name = info.baseName();
        for (const QByteArray& raw : f.readAll().split('\n'))
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

    Finalize(*set);
    if (!set->SymbolCount)
    {
        error = "no symbols in " + path;
        return false;
    }
    Base = set;
    rebuild();
    Loaded.clear();
    return true;
}

bool DebugSymbols::loadLabels(const QString& path, QString& error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        error = "cannot read " + path;
        return false;
    }
    std::vector<Label> labels;
    for (QByteArray line : f.readAll().split('\n'))
    {
        int hash = line.indexOf('#');
        if (hash >= 0)
            line.truncate(hash);
        QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 2)
            continue;
        QByteArray addrText = parts[0];
        if (addrText.startsWith("0x") || addrText.startsWith("0X"))
            addrText = addrText.mid(2);
        bool ok;
        u32 addr = addrText.toUInt(&ok, 16);
        if (!ok)
            continue;
        labels.push_back({addr, QString::fromUtf8(parts[1]), parts.size() > 2 ? QString::fromUtf8(parts[2]) : QString()});
    }
    if (labels.empty())
    {
        error = "no labels in " + path;
        return false;
    }
    Labels = std::move(labels);
    LabelSource = path;
    rebuild();
    return true;
}

void DebugSymbols::clearLabels()
{
    Labels.clear();
    LabelSource.clear();
    rebuild();
}

void DebugSymbols::rebuild()
{
    Unmatched = 0;
    if (Labels.empty())
    {
        Cur = Base;
        return;
    }
    auto set = Base ? std::make_shared<Set>(*Base) : std::make_shared<Set>();
    if (!Base)
    {
        set->Source = LabelSource;
        set->Modules.push_back(Module{"labels"});
    }
    for (const Label& label : Labels)
    {
        u32 addr = label.Addr & ~1u;
        int m = -1;
        for (int i = 0; i < (int)set->Modules.size() && m < 0; i++)
        {
            const Module& mod = set->Modules[i];
            if (!label.Module.isEmpty() ? mod.Name.compare(label.Module, Qt::CaseInsensitive) == 0
                                        : mod.Overlay < 0 && mod.covers(addr))
                m = i;
        }
        if (m < 0 && label.Module.isEmpty() && !Base)
            m = 0;
        if (m < 0)
        {
            Unmatched++;
            continue;
        }
        Module& mod = set->Modules[m];
        auto it = std::find_if(mod.Symbols.begin(), mod.Symbols.end(), [addr](const Symbol& s) { return s.Addr == addr; });
        if (it != mod.Symbols.end())
        {
            if (it->Original.isEmpty())
                it->Original = it->Name;
            it->Name = label.Name;
        }
        else
        {
            Symbol sym {};
            sym.Addr = addr;
            sym.Thumb = label.Addr & 1;
            sym.Name = label.Name;
            sym.Kind = "label";
            sym.Module = m;
            mod.Symbols.push_back(std::move(sym));
        }
    }
    Finalize(*set);
    Cur = set;
}

void DebugSymbols::clear()
{
    Base.reset();
    Cur.reset();
    Labels.clear();
    LabelSource.clear();
    Loaded.clear();
}

std::vector<int> DebugSymbols::detect(const Set& set, NDS& nds)
{
    std::vector<int> out;
    for (const Module& mod : set.Modules)
    {
        if (mod.Overlay < 0 || mod.Image.isEmpty())
            continue;
        u32 code = std::min<u32>(mod.Image.size(), mod.Size);
        int hits = 0;
        for (int w = 0; w < SampleWindows; w++)
        {
            u32 off = (u32)((u64)code * (2 * w + 1) / (2 * SampleWindows)) & ~3u;
            bool same = true;
            for (u32 b = 0; b < SampleBytes && off + b + 4 <= (u32)mod.Image.size(); b += 4)
            {
                u32 want;
                memcpy(&want, mod.Image.constData() + off + b, 4);
                if (nds.Debugger.Peek(0, mod.Base + off + b, 4) != want)
                {
                    same = false;
                    break;
                }
            }
            hits += same;
        }
        if (hits * 4 >= SampleWindows * 3)
            out.push_back(mod.Overlay);
    }
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
