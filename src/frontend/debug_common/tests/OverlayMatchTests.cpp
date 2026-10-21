// MatchOverlay (src/debug/Probes.cpp): which overlay's code is at an address, judged from memory.
#include <cstring>
#include <map>
#include "Harness.h"
#include "debug/Probes.h"

using namespace melonDS;

namespace
{

std::vector<u8> Words(std::initializer_list<u32> words)
{
    std::vector<u8> out(words.size() * 4);
    size_t i = 0;
    for (u32 w : words)
        memcpy(&out[4 * i++], &w, 4);
    return out;
}

// memory holding image at base (and 0 elsewhere)
struct Memory
{
    std::map<u32, u32> Cells;
    void Put(u32 base, const std::vector<u8>& image)
    {
        for (size_t i = 0; i + 4 <= image.size(); i += 4)
        {
            u32 w;
            memcpy(&w, &image[i], 4);
            Cells[base + i] = w;
        }
    }
    std::function<u32(u32)> Peek() const
    {
        return [this](u32 a) { auto it = Cells.find(a); return it == Cells.end() ? 0u : it->second; };
    }
};

}

TEST(OverlayMatchPicksTheResidentOne)
{
    const u32 base = 0x02100000;
    std::vector<RomOverlay> ovs = {
        {3, base, 0x40, Words({1, 2, 3, 4, 5, 6, 7, 8})},
        {5, base, 0x40, Words({9, 10, 11, 12, 13, 14, 15, 16})},
    };
    Memory m;
    m.Put(base, ovs[1].Image);
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base + 8, m.Peek()), 5);
    m.Put(base, ovs[0].Image);
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base + 8, m.Peek()), 3);
    CHECK_EQ(MatchOverlay(ovs, {}, base, m.Peek()), OverlayStatic);
}

TEST(OverlayMatchToleratesAPatchedWord)
{
    const u32 base = 0x02100000;
    std::vector<RomOverlay> ovs = {
        {3, base, 0x40, Words({1, 2, 3, 4, 5, 6, 7, 8})},
        {5, base, 0x40, Words({9, 10, 11, 12, 13, 14, 15, 16})},
    };
    Memory m;
    m.Put(base, ovs[1].Image);
    m.Cells[base + 8] = 0xE12FFF1E;     // a cheat code patched the instruction there
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base + 8, m.Peek()), 5);
}

TEST(OverlayMatchUnknownAndAmbiguous)
{
    const u32 base = 0x02100000;
    // the same code at the address in both, different starts
    std::vector<RomOverlay> ovs = {
        {3, base, 0x40, Words({1, 2, 3, 4, 7, 7, 7, 7})},
        {5, base, 0x40, Words({9, 10, 11, 12, 7, 7, 7, 7})},
    };
    Memory m;
    m.Put(base, ovs[1].Image);
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base + 16, m.Peek()), 5);      // the start decides
    m.Put(base, Words({20, 21, 22, 23, 7, 7, 7, 7}));
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base + 16, m.Peek()), OverlayAmbiguous);
    m.Put(base, Words({30, 31, 32, 33, 34, 35, 36, 37}));
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base + 16, m.Peek()), OverlayUnknown);
}

TEST(OverlayMatchDifferentSizes)
{
    // a small overlay loaded over a bigger one leaves the bigger one's tail: past the small one's code only the big
    // one is a candidate, and the tail still matches it (that is the code there)
    const u32 base = 0x02100000;
    std::vector<RomOverlay> ovs = {
        {3, base, 0x20, Words({1, 2, 3, 4, 5, 6, 7, 8})},
        {5, base, 0x08, Words({9, 10})},
    };
    Memory m;
    m.Put(base, ovs[0].Image);
    m.Put(base, ovs[1].Image);
    CHECK_EQ(MatchOverlay(ovs, {0, 1}, base, m.Peek()), 5);
    CHECK_EQ(MatchOverlay(ovs, {0}, base + 0x10, m.Peek()), 3);
}
