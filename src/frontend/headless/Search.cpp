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

#include "Search.h"

#include <algorithm>
#include <cstring>

#include "Protocol.h"

namespace Headless::Search
{

// Offsets (ascending) at which needle occurs; stops once `want` are found.
static std::vector<size_t> Scan(const uint8_t* data, size_t len, const uint8_t* needle, size_t n, size_t step,
                                size_t first, size_t want)
{
    std::vector<size_t> found;
    if (n == 0 || len < n || want == 0)
        return found;
    size_t last = len - n;
    for (size_t i = first; i <= last;)
    {
        if (step == 1)
        {
            const void* p = std::memchr(data + i, needle[0], last - i + 1);
            if (!p)
                break;
            i = size_t((const uint8_t*)p - data);
        }
        if (std::memcmp(data + i, needle, n) == 0)
        {
            found.push_back(i);
            if (found.size() >= want)
                break;
        }
        i += step;
    }
    return found;
}

ValueHits FindValue(const uint8_t* data, size_t len, uint32_t base, unsigned size,
                    const uint8_t* needle, size_t needleLen, uint32_t limit)
{
    ValueHits out;
    size_t n = size ? size : needleLen;
    size_t step = size ? size : 1;
    size_t first = size ? (size - base % size) % size : 0;
    for (size_t off : Scan(data, len, needle, n, step, first, size_t(limit) + 1))
    {
        if (out.Addrs.size() == limit)
        {
            out.Truncated = true;
            break;
        }
        out.Addrs.push_back(base + uint32_t(off));
    }
    return out;
}

static bool DecodeUtf8(const std::string& s, std::vector<uint32_t>& cps)
{
    for (size_t i = 0; i < s.size();)
    {
        uint8_t c = uint8_t(s[i]);
        uint32_t cp, extra, min;
        if (c < 0x80) { cp = c; extra = 0; min = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; min = 0x10000; }
        else return false;
        for (uint32_t k = 1; k <= extra; k++)
        {
            if (i + k >= s.size() || (uint8_t(s[i + k]) & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        cps.push_back(cp);
        i += extra + 1;
    }
    return true;
}

bool EncodeText(const std::string& utf8, uint8_t mask, std::vector<EncodedText>& out)
{
    std::vector<uint32_t> cps;
    if (!DecodeUtf8(utf8, cps))
        return false;
    bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) { return uint8_t(c) < 0x80; });
    bool wantAscii = (mask & Protocol::EncAscii) && ascii;
    if (wantAscii)
        out.push_back({Protocol::EncAscii, std::vector<uint8_t>(utf8.begin(), utf8.end())});
    if (mask & Protocol::EncUtf16le)
    {
        EncodedText t {Protocol::EncUtf16le, {}};
        auto unit = [&](uint32_t u) { t.Bytes.push_back(uint8_t(u)); t.Bytes.push_back(uint8_t(u >> 8)); };
        for (uint32_t cp : cps)
        {
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                unit(0xD800 + (cp >> 10));
                unit(0xDC00 + (cp & 0x3FF));
            }
            else
                unit(cp);
        }
        out.push_back(std::move(t));
    }
    if ((mask & Protocol::EncUtf8) && !(wantAscii && ascii))
        out.push_back({Protocol::EncUtf8, std::vector<uint8_t>(utf8.begin(), utf8.end())});
    return true;
}

TextHits FindText(const uint8_t* data, size_t len, uint32_t base, const std::vector<EncodedText>& patterns,
                  uint32_t limit)
{
    std::vector<TextHit> all;
    for (const EncodedText& p : patterns)
        for (size_t off : Scan(data, len, p.Bytes.data(), p.Bytes.size(), 1, 0, size_t(limit) + 1))
            all.push_back({base + uint32_t(off), p.Encoding});
    std::sort(all.begin(), all.end(), [](const TextHit& a, const TextHit& b) {
        return a.Addr != b.Addr ? a.Addr < b.Addr : a.Encoding < b.Encoding;
    });
    TextHits out;
    if (all.size() > limit)
    {
        out.Truncated = true;
        all.resize(limit);
    }
    out.Hits = std::move(all);
    return out;
}

}
