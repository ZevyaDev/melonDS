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

#include "Cheats.h"

#include <cstdio>

namespace Headless
{

namespace
{

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseWord(const std::string& token, uint32_t& out)
{
    if (token.size() != 8)
        return false;
    uint32_t v = 0;
    for (char c : token)
    {
        int d = HexDigit(c);
        if (d < 0)
            return false;
        v = (v << 4) | (uint32_t)d;
    }
    out = v;
    return true;
}

std::string Shown(const std::string& line)
{
    std::string s = line.size() > 40 ? line.substr(0, 40) + "..." : line;
    return "\"" + s + "\"";
}

std::string Hex2(uint32_t v)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", v & 0xFF);
    return buf;
}

struct Pair
{
    uint32_t A, B;
    int Line;
};

}

bool ParseCheatCode(const std::string& text, std::vector<uint32_t>& words, std::string& error)
{
    std::vector<Pair> pairs;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        lineNo++;

        std::vector<std::string> tokens;
        std::string cur;
        for (char c : line)
        {
            if (IsSpace(c))
            {
                if (!cur.empty())
                    tokens.push_back(cur), cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty())
            tokens.push_back(cur);
        if (tokens.empty())
            continue;

        Pair p {0, 0, lineNo};
        if (tokens.size() != 2 || !ParseWord(tokens[0], p.A) || !ParseWord(tokens[1], p.B))
        {
            error = "line " + std::to_string(lineNo) + ": expected two 8-digit hex words (\"XXXXXXXX YYYYYYYY\"), got "
                    + Shown(line);
            return false;
        }
        pairs.push_back(p);
        if (pairs.size() * 2 > MaxCheatWords)
        {
            error = "the code is longer than " + std::to_string(MaxCheatWords / 2) + " lines";
            return false;
        }
    }
    if (pairs.empty())
    {
        error = "the code is empty (no \"XXXXXXXX YYYYYYYY\" lines)";
        return false;
    }

    // Check the opcodes as melonDS's AREngine (src/AREngine.cpp) reads them.
    for (size_t i = 0; i < pairs.size(); i++)
    {
        const Pair& p = pairs[i];
        uint32_t op = p.A >> 24;
        std::string where = "line " + std::to_string(p.Line) + ": ";
        if (op < 0xC0 || op >= 0xF0 || op == 0xC0 || op == 0xC5 || op == 0xC6)
            continue;
        if (op == 0xC4)
        {
            error = where + "opcode C4 (offset = address of the code) is not supported by melonDS's AR engine";
            return false;
        }
        if (op >= 0xD0 && op <= 0xDC)
        {
            if (op == 0xD4 && (p.A & 0xFF) > 0x08)
            {
                error = where + "unknown D4 data operation " + Hex2(p.A) + " (00-08 exist)";
                return false;
            }
            continue;
        }
        if ((op & 0xF0) == 0xE0)
        {
            // E: copy B bytes of parameter data that follow, 8 bytes per line (a partial last line is padded)
            size_t need = ((size_t)p.B + 7) / 8;
            size_t have = pairs.size() - i - 1;
            if (need > have)
            {
                error = where + "the E opcode copies " + std::to_string(p.B) + " bytes (" + std::to_string(need)
                        + " lines of data) but only " + std::to_string(have) + " lines follow";
                return false;
            }
            i += need;
            continue;
        }
        error = where + "unknown opcode " + Hex2(op) + " (melonDS's AR engine would stop there)";
        return false;
    }

    words.clear();
    for (const Pair& p : pairs)
    {
        words.push_back(p.A);
        words.push_back(p.B);
    }
    return true;
}

}
