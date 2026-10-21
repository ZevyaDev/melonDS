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
#include <string.h>

#include "RomOverlays.h"
#include "../NDS_Header.h"

namespace melonDS
{

constexpr int SampleWindows = 8;
constexpr int SampleBytes = 16;

bool BlzDecompress(std::vector<u8>& data)
{
    u32 len = (u32)data.size();
    if (len < 8)
        return false;
    const u8* src = data.data();
    u32 footer, extra;
    memcpy(&footer, src + len - 8, 4);
    memcpy(&extra, src + len - 4, 4);
    if (!extra)
        return true;
    u32 packed = footer & 0xFFFFFF;
    u32 header = footer >> 24;
    if (packed > len || header > packed || header < 8)
        return false;
    std::vector<u8> out(len + extra);
    memcpy(out.data(), src, len);
    u32 in = len - header;
    u32 stop = len - packed;
    u32 pos = len + extra;
    while (in > stop)
    {
        u8 flags = src[--in];
        for (int bit = 0; bit < 8 && in > stop; bit++, flags <<= 1)
        {
            if (!(flags & 0x80))
            {
                if (pos == 0)
                    return false;
                out[--pos] = src[--in];
                continue;
            }
            if (in < stop + 2)
                return false;
            u32 hi = src[--in];
            u32 lo = src[--in];
            u32 count = (hi >> 4) + 3;
            u32 disp = (((hi & 0xF) << 8) | lo) + 3;
            for (u32 k = 0; k < count; k++)
            {
                if (pos == 0 || pos - 1 + disp >= out.size())
                    return false;
                out[pos - 1] = out[pos - 1 + disp];
                pos--;
            }
        }
    }
    data = std::move(out);
    return true;
}

std::vector<RomOverlay> ReadRomOverlays(const u8* rom, u32 len, const NDSHeader& h)
{
    std::vector<RomOverlay> out;
    if (!rom)
        return out;
    auto word = [&](u32 off) {
        u32 v = 0;
        if ((u64)off + 4 <= len)
            memcpy(&v, rom + off, 4);
        return v;
    };
    for (u32 i = 0; i < h.ARM9OverlaySize / 32; i++)
    {
        u32 at = h.ARM9OverlayOffset + i * 32;
        if ((u64)at + 32 > len)
            break;
        u32 fat = h.FATOffset + word(at + 24) * 8;
        u32 start = word(fat), end = word(fat + 4);
        if (end <= start || end > len)
            continue;
        std::vector<u8> image(rom + start, rom + end);
        u32 flags = word(at + 28);
        if ((flags >> 24) & 1)
        {
            u32 packed = flags & 0xFFFFFF;
            if (packed && packed < image.size())
                image.resize(packed);
            if (!BlzDecompress(image))
                continue;
        }
        out.push_back({(int)word(at), word(at + 4), word(at + 8) + word(at + 12), std::move(image)});
    }
    return out;
}

bool OverlayLoaded(const std::function<u32(u32)>& peek32, u32 base, u32 size, const std::vector<u8>& image)
{
    if (image.empty())
        return false;
    u32 code = std::min<u32>(image.size(), size);
    int hits = 0;
    for (int w = 0; w < SampleWindows; w++)
    {
        u32 off = (u32)((u64)code * (2 * w + 1) / (2 * SampleWindows)) & ~3u;
        bool same = true;
        for (u32 b = 0; b < SampleBytes && off + b + 4 <= image.size(); b += 4)
        {
            u32 want;
            memcpy(&want, image.data() + off + b, 4);
            if (peek32(base + off + b) != want)
            {
                same = false;
                break;
            }
        }
        hits += same;
    }
    return hits * 4 >= SampleWindows * 3;
}

}
