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

// MatchOverlay (Probes.h) alone, without the emulator, so debug_common_tests can link it.

#include <cstring>

#include "Probes.h"

namespace melonDS
{

int MatchOverlay(const std::vector<RomOverlay>& overlays, const std::vector<int>& candidates, u32 addr,
                 const std::function<u32(u32)>& peek32)
{
    if (candidates.empty())
        return OverlayStatic;
    // equal words / compared words in a window of `words` words from `from`, within the overlay's code
    auto score = [&](int idx, u32 from, u32 words) {
        const RomOverlay& ov = overlays[idx];
        u32 eq = 0, n = 0;
        for (u32 w = 0; w < words; w++)
        {
            u32 x = (from & ~3u) + 4 * w;
            if (x < ov.Base)
                continue;
            u32 off = x - ov.Base;
            if (off + 4 > ov.Image.size())
                break;
            u32 want;
            memcpy(&want, ov.Image.data() + off, 4);
            eq += peek32(x) == want;
            n++;
        }
        return std::pair<u32, u32>(eq, n);
    };
    // the candidates whose score is highest; empty when none matches at least half its words
    auto best = [&](const std::vector<int>& among, auto from, u32 words) {
        std::vector<int> top;
        u32 topEq = 0, topN = 1;
        for (int idx : among)
        {
            auto [eq, n] = score(idx, from(idx), words);
            if (n == 0 || eq * 2 < n)
                continue;
            u64 lhs = (u64)eq * topN, rhs = (u64)topEq * n;
            if (top.empty() || lhs > rhs)
            {
                top = {idx};
                topEq = eq;
                topN = n;
            }
            else if (lhs == rhs)
                top.push_back(idx);
        }
        return top;
    };
    auto at = [addr](int) { return addr; };
    std::vector<int> top = best(candidates, at, 16);
    if (top.size() != 1)
    {
        std::vector<int> wide = best(candidates, at, 64);
        if (!wide.empty())
            top = wide;
    }
    if (top.empty())
        return OverlayUnknown;
    if (top.size() > 1)
    {
        // still tied: the code at each overlay's own start (the region start rule)
        std::vector<int> start = best(top, [&](int idx) { return overlays[idx].Base; }, 8);
        if (start.size() != 1)
            return OverlayAmbiguous;
        top = start;
    }
    return overlays[top[0]].Id;
}

}
