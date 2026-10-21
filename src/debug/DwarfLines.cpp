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

#include <string.h>

#include "DwarfLines.h"

namespace melonDS
{

namespace
{

struct Reader
{
    const u8* Data;
    size_t End;
    size_t Pos;
    bool Bad = false;

    bool has(size_t n) { if (Pos + n > End) Bad = true; return !Bad; }
    u8 byte() { return has(1) ? Data[Pos++] : 0; }
    u16 half() { if (!has(2)) return 0; u16 v; memcpy(&v, Data + Pos, 2); Pos += 2; return v; }
    u32 word() { if (!has(4)) return 0; u32 v; memcpy(&v, Data + Pos, 4); Pos += 4; return v; }
    u64 uleb()
    {
        u64 v = 0;
        for (int shift = 0; has(1); shift += 7)
        {
            u8 b = Data[Pos++];
            if (shift < 64)
                v |= (u64)(b & 0x7F) << shift;
            if (!(b & 0x80))
                break;
        }
        return v;
    }
    s64 sleb()
    {
        s64 v = 0;
        int shift = 0;
        u8 b = 0;
        while (has(1))
        {
            b = Data[Pos++];
            if (shift < 64)
                v |= (s64)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        if (shift < 64 && (b & 0x40))
            v |= -((s64)1 << shift);
        return v;
    }
    std::string str()
    {
        size_t start = Pos;
        while (has(1) && Data[Pos])
            Pos++;
        std::string s((const char*)Data + start, Pos - start);
        if (has(1))
            Pos++;
        return s;
    }
};

std::string JoinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty() || name.empty() || name[0] == '/' || name[0] == '\\' || (name.size() > 1 && name[1] == ':'))
        return name;
    char last = dir.back();
    return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

bool ReadProgram(Reader& r, DwarfLineProgram& prog, std::string& error)
{
    u32 unitLength = r.word();
    if (unitLength >= 0xFFFFFFF0)
    {
        error = "64-bit DWARF is not supported";
        return false;
    }
    if (!r.has(unitLength))
        return false;
    size_t unitEnd = r.Pos + unitLength;
    u16 version = r.half();
    if (version < 2 || version > 4)
    {
        error = "unsupported .debug_line version " + std::to_string(version);
        return false;
    }
    u32 headerLength = r.word();
    size_t programStart = r.Pos + headerLength;
    u8 minInst = r.byte();
    if (version >= 4)
        r.byte();
    bool defaultStmt = r.byte() != 0;
    s8 lineBase = (s8)r.byte();
    u8 lineRange = r.byte();
    u8 opcodeBase = r.byte();
    if (!lineRange || !opcodeBase)
    {
        error = "bad .debug_line header";
        return false;
    }
    std::vector<u8> argCounts(opcodeBase, 0);
    for (int i = 1; i < opcodeBase; i++)
        argCounts[i] = r.byte();
    std::vector<std::string> dirs {""};
    for (std::string d = r.str(); !d.empty() && !r.Bad; d = r.str())
        dirs.push_back(d);
    prog.Files = {""};
    for (std::string name = r.str(); !name.empty() && !r.Bad; name = r.str())
    {
        u64 dir = r.uleb();
        r.uleb();
        r.uleb();
        prog.Files.push_back(JoinPath(dir < dirs.size() ? dirs[dir] : "", name));
    }
    if (r.Bad || programStart > unitEnd)
        return false;
    r.Pos = programStart;

    u32 addr = 0, file = 1, line = 1;
    bool stmt = defaultStmt;
    auto emit = [&](bool end) { prog.Rows.push_back({addr, line, file, stmt, end}); };
    auto reset = [&] { addr = 0; file = 1; line = 1; stmt = defaultStmt; };
    while (r.Pos < unitEnd && !r.Bad)
    {
        u8 op = r.byte();
        if (op >= opcodeBase)
        {
            u8 adj = op - opcodeBase;
            addr += (adj / lineRange) * minInst;
            line += lineBase + adj % lineRange;
            emit(false);
            continue;
        }
        switch (op)
        {
        case 0:
        {
            u64 len = r.uleb();
            size_t next = r.Pos + len;
            u8 sub = len ? r.byte() : 0;
            if (sub == 1)
            {
                emit(true);
                reset();
            }
            else if (sub == 2)
                addr = len >= 5 ? r.word() : addr;
            else if (sub == 3)
            {
                std::string name = r.str();
                u64 dir = r.uleb();
                prog.Files.push_back(JoinPath(dir < dirs.size() ? dirs[dir] : "", name));
            }
            r.Pos = next;
            break;
        }
        case 1: emit(false); break;
        case 2: addr += (u32)r.uleb() * minInst; break;
        case 3: line += (s32)r.sleb(); break;
        case 4: file = (u32)r.uleb(); break;
        case 6: stmt = !stmt; break;
        case 8: addr += ((255 - opcodeBase) / lineRange) * minInst; break;
        case 9: addr += r.half(); break;
        default:
            for (int i = 0; i < argCounts[op]; i++)
                r.uleb();
            break;
        }
    }
    r.Pos = unitEnd;
    return true;
}

}

bool ReadDwarfLines(const u8* elf, size_t len, std::vector<DwarfLineProgram>& out, std::string& error)
{
    Reader r {elf, len, 0};
    if (len < 0x34 || memcmp(elf, "\x7F" "ELF", 4) != 0 || elf[4] != 1 || elf[5] != 1)
    {
        error = "not a 32-bit little-endian ELF file";
        return false;
    }
    r.Pos = 0x20;
    u32 shoff = r.word();
    r.Pos = 0x2E;
    u16 shentsize = r.half();
    u16 shnum = r.half();
    u16 shstrndx = r.half();
    if (shentsize < 40 || shstrndx >= shnum || (u64)shoff + (u64)shnum * shentsize > len)
    {
        error = "bad ELF section headers";
        return false;
    }
    auto header = [&](u32 i, u32 field) {
        u32 v;
        memcpy(&v, elf + shoff + i * shentsize + field, 4);
        return v;
    };
    u32 strOff = header(shstrndx, 16), strSize = header(shstrndx, 20);
    if ((u64)strOff + strSize > len)
    {
        error = "bad ELF section names";
        return false;
    }
    for (u32 i = 0; i < shnum; i++)
    {
        u32 name = header(i, 0);
        if (name >= strSize || strncmp((const char*)elf + strOff + name, ".debug_line", strSize - name) != 0)
            continue;
        u32 off = header(i, 16), size = header(i, 20);
        if ((u64)off + size > len)
        {
            error = "bad .debug_line section";
            return false;
        }
        Reader lines {elf, (size_t)off + size, off};
        while (lines.Pos < lines.End)
        {
            DwarfLineProgram prog;
            if (!ReadProgram(lines, prog, error))
            {
                if (error.empty())
                    error = "truncated .debug_line";
                return false;
            }
            if (!prog.Rows.empty())
                out.push_back(std::move(prog));
        }
        return true;
    }
    error = "no .debug_line section: link with -sym on";
    return false;
}

}
