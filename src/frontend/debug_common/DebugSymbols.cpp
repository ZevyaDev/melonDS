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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "DebugSymbols.h"
#include "NDS.h"

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define HAVE_CXXABI
#endif

using namespace melonDS;

namespace fs = std::filesystem;

namespace
{

std::string DemangleName(const std::string& name)
{
#ifdef HAVE_CXXABI
    if (name.compare(0, 2, "_Z") != 0)
        return name;
    int status = 0;
    char* out = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !out)
        return name;
    std::string pretty = out;
    free(out);
    return pretty;
#else
    return name;
#endif
}

bool ReadFile(const std::string& path, std::string& out)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> SplitLines(const std::string& data)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos)
        {
            out.push_back(data.substr(start));
            break;
        }
        out.push_back(data.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// Directory part of a path, '/'-separated; empty for a bare file name
std::string DirName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool IsAbsolute(const std::string& path)
{
    return fs::u8path(path).is_absolute();
}

std::string JoinPath(const std::string& dir, const std::string& rel)
{
    if (dir.empty() || IsAbsolute(rel))
        return rel;
    if (dir.back() == '/' || dir.back() == '\\')
        return dir + rel;
    return dir + "/" + rel;
}

// Text after the last '.' of the file name, and before the first '.'
std::string Suffix(const std::string& path)
{
    std::string name = fs::u8path(path).filename().u8string();
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : name.substr(dot + 1);
}

std::string BaseName(const std::string& path)
{
    std::string name = fs::u8path(path).filename().u8string();
    size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Whole-string unsigned number, rejecting signs, spaces and overflow past 32 bits
bool ToU32(const std::string& s, int base, u32& out)
{
    if (s.empty() || !isalnum((unsigned char)s[0]))
        return false;
    char* end = nullptr;
    unsigned long long v = strtoull(s.c_str(), &end, base);
    if (*end || v > 0xFFFFFFFFull)
        return false;
    out = (u32)v;
    return true;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string Simplify(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (IsSpace(c))
        {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
        }
        else
            out += c;
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
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

bool ParseYAML(const std::string& path, YNode& out)
{
    std::string data;
    if (!ReadFile(path, data))
        return false;
    std::vector<YLine> lines;
    for (const std::string& s : SplitLines(data))
    {
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

void ParseDsdSymbols(const std::string& path, int module, std::vector<DebugSymbols::Symbol>& out)
{
    std::string data;
    if (!ReadFile(path, data))
        return;
    for (const std::string& raw : SplitLines(data))
    {
        std::string line = Trim(raw);
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos)
            continue;
        size_t kindPos = line.find("kind:", sp);
        size_t addrPos = line.find("addr:0x", sp);
        if (kindPos == std::string::npos || addrPos == std::string::npos)
            continue;
        DebugSymbols::Symbol sym {};
        sym.Name = line.substr(0, sp);
        sym.Module = module;
        size_t kindEnd = kindPos + 5;
        while (kindEnd < line.size() && (isalnum((unsigned char)line[kindEnd]) || line[kindEnd] == '_'))
            kindEnd++;
        sym.Kind = line.substr(kindPos + 5, kindEnd - kindPos - 5);
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

bool DebugSymbols::load(const std::string& rawPath, std::string& error)
{
    std::string path = rawPath;
    std::error_code ec;
    if (fs::is_directory(fs::u8path(path), ec))
        path = JoinPath(path, "config.yaml");
    if (!fs::exists(fs::u8path(path), ec))
    {
        error = "not found: " + path;
        return false;
    }

    auto set = std::make_shared<Set>();
    set->Source = path;

    std::string suffix = Suffix(path);
    if (suffix == "yaml" || suffix == "yml")
    {
        YNode cfg;
        if (!ParseYAML(path, cfg))
        {
            error = "cannot read " + path;
            return false;
        }
        std::string root = DirName(path);
        auto add = [&](const std::string& name, int overlay, const std::string& symbols) {
            Module mod;
            mod.Name = name;
            mod.Overlay = overlay;
            ParseDsdSymbols(JoinPath(root, symbols), (int)set->Modules.size(), mod.Symbols);
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
                add(a.str("name"), -1, a.str("symbols"));

        std::unordered_map<int, std::pair<YNode, std::string>> table;
        std::string romCfg = cfg.str("rom_config");
        if (!romCfg.empty())
        {
            std::string romDir = DirName(JoinPath(root, romCfg));
            std::string tablePath = JoinPath(romDir, "arm9_overlays/overlays.yaml");
            YNode ovTable;
            if (ParseYAML(tablePath, ovTable))
                if (const YNode* list = ovTable.get("overlays"))
                    for (const YNode& ov : list->List)
                        table.insert_or_assign((int)ov.num("id"), std::make_pair(ov, JoinPath(romDir, "arm9_overlays/" + ov.str("file_name"))));
        }
        if (const YNode* overlays = cfg.get("overlays"))
            for (const YNode& ov : overlays->List)
            {
                int id = (int)ov.num("id");
                add(ov.str("name"), id, ov.str("symbols"));
                Module& mod = set->Modules.back();
                auto it = table.find(id);
                if (it != table.end())
                {
                    mod.Base = it->second.first.num("base_address");
                    mod.Size = it->second.first.num("code_size") + it->second.first.num("bss_size");
                    std::string image;
                    if (ReadFile(it->second.second, image))
                        mod.Image.assign(image.begin(), image.end());
                }
            }
    }
    else
    {
        std::string data;
        if (!ReadFile(path, data))
        {
            error = "cannot read " + path;
            return false;
        }
        Module mod;
        mod.Name = BaseName(path);
        for (const std::string& raw : SplitLines(data))
        {
            std::string line = Simplify(raw);
            size_t sp1 = line.find(' ');
            if (sp1 == std::string::npos)
                continue;
            u32 addr;
            if (!ToU32(line.substr(0, sp1), 16, addr))
                continue;
            size_t sp2 = line.find(' ', sp1 + 1);
            Symbol sym {};
            sym.Addr = addr & ~1u;
            sym.Thumb = addr & 1;
            sym.Name = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
            sym.Kind = "function";
            if (sp2 != std::string::npos)
            {
                size_t sp3 = line.find(' ', sp2 + 1);
                u32 size = 0;
                if (ToU32(line.substr(sp2 + 1, sp3 == std::string::npos ? std::string::npos : sp3 - sp2 - 1), 0, size))
                    sym.Size = size;
            }
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
            set->ByName[sym.Name].push_back({m, s});
            if (sym.Pretty != sym.Name)
            {
                set->ByName[sym.Pretty].push_back({m, s});
                size_t paren = sym.Pretty.find('(');
                if (paren != std::string::npos && paren > 0)
                    set->ByName[sym.Pretty.substr(0, paren)].push_back({m, s});
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
    return true;
}

void DebugSymbols::clear()
{
    Cur.reset();
    Loaded.clear();
}

std::vector<int> DebugSymbols::detect(const Set& set, const std::function<u32(u32)>& peek)
{
    std::vector<int> out;
    for (const Module& mod : set.Modules)
    {
        if (mod.Overlay < 0 || mod.Image.empty())
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
                memcpy(&want, mod.Image.data() + off + b, 4);
                if (peek(mod.Base + off + b) != want)
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

std::vector<int> DebugSymbols::detect(const Set& set, NDS& nds)
{
    return detect(set, [&nds](u32 addr) -> u32 { return nds.Debugger.Peek(0, addr, 4); });
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

std::string DebugSymbols::describe(u32 addr, bool withModule) const
{
    const Symbol* s = lookup(addr);
    if (!s)
        return std::string();
    u32 off = (addr & ~1u) - s->Addr;
    std::string out = name(*s);
    if (off)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "+0x%x", off);
        out += buf;
    }
    const Module* mod = moduleOf(s);
    if (withModule && mod && mod->Overlay >= 0)
        out += " [" + mod->Name + "]";
    return out;
}

std::string DebugSymbols::exact(u32 addr) const
{
    const Symbol* s = lookup(addr);
    if (s && s->Addr == (addr & ~1u))
        return name(*s);
    return std::string();
}

bool DebugSymbols::resolve(const std::string& name, u32& out) const
{
    if (!Cur)
        return false;
    auto found = Cur->ByName.find(name);
    if (found == Cur->ByName.end() || found->second.empty())
        return false;
    const auto& hits = found->second;
    // most recently added first, as the old QMultiHash::values order
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
    {
        const Module& mod = Cur->Modules[it->first];
        if (mod.Overlay < 0 || std::find(Loaded.begin(), Loaded.end(), mod.Overlay) != Loaded.end())
        {
            out = mod.Symbols[it->second].Addr;
            return true;
        }
    }
    out = Cur->Modules[hits.back().first].Symbols[hits.back().second].Addr;
    return true;
}

std::vector<const DebugSymbols::Symbol*> DebugSymbols::search(const std::string& text, int limit) const
{
    std::vector<const Symbol*> out;
    if (!Cur)
        return out;
    std::string needle = Lower(text);
    for (const Module& mod : Cur->Modules)
        for (const Symbol& s : mod.Symbols)
            if (Lower(s.Name).find(needle) != std::string::npos || Lower(s.Pretty).find(needle) != std::string::npos)
            {
                out.push_back(&s);
                if ((int)out.size() >= limit)
                    return out;
            }
    return out;
}

bool DebugSymbols::parseAddress(const std::string& raw, u32& out) const
{
    auto trimmed = [](const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && IsSpace(s[a]))
            a++;
        while (b > a && IsSpace(s[b - 1]))
            b--;
        return s.substr(a, b - a);
    };
    auto stripHex = [](const std::string& s) {
        return (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? s.substr(2) : s;
    };

    std::string text = trimmed(raw);
    if (text.empty())
        return false;
    size_t plus = text.rfind('+');
    u32 offset = 0;
    if (plus != std::string::npos && plus > 0)
    {
        std::string off = trimmed(text.substr(plus + 1));
        if (ToU32(stripHex(off), 16, offset))
            text = trimmed(text.substr(0, plus));
        else
            offset = 0;
    }
    if (resolve(text, out))
    {
        out += offset;
        return true;
    }
    u32 value;
    bool ok = ToU32(stripHex(text), 16, value);
    out = (ok ? value : 0) + offset;
    return ok;
}
