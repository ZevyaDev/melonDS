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

#include "TestHarness.h"
#include "Protocol.h"
#include "Search.h"

using namespace Headless;
using namespace Headless::Search;

static std::vector<uint8_t> Mem(size_t n)
{
    return std::vector<uint8_t>(n, 0);
}

static void Put(std::vector<uint8_t>& m, size_t at, const std::string& s)
{
    for (size_t i = 0; i < s.size(); i++)
        m[at + i] = uint8_t(s[i]);
}

TEST(search_u32_aligned_by_absolute_address)
{
    auto m = Mem(64);
    Put(m, 8, std::string("\x78\x56\x34\x12", 4));
    Put(m, 21, std::string("\x78\x56\x34\x12", 4));      // unaligned
    uint8_t needle[4] = {0x78, 0x56, 0x34, 0x12};
    auto r = FindValue(m.data(), m.size(), 0x02000000, 4, needle, 4, 10);
    CHECK_EQ(r.Addrs.size(), size_t(1));
    CHECK_EQ(r.Addrs[0], 0x02000008u);
    CHECK(!r.Truncated);
    // the same bytes seen from an unaligned base: alignment is of the guest address
    auto r2 = FindValue(m.data() + 1, m.size() - 1, 0x02000001, 4, needle, 4, 10);
    CHECK_EQ(r2.Addrs.size(), size_t(1));
    CHECK_EQ(r2.Addrs[0], 0x02000008u);
}

TEST(search_u16_and_u8)
{
    auto m = Mem(32);
    m[4] = 0xCD; m[5] = 0xAB;
    m[11] = 0xCD; m[12] = 0xAB;                          // unaligned
    uint8_t n16[2] = {0xCD, 0xAB};
    auto r = FindValue(m.data(), m.size(), 0, 2, n16, 2, 10);
    CHECK_EQ(r.Addrs.size(), size_t(1));
    CHECK_EQ(r.Addrs[0], 4u);
    uint8_t n8[1] = {0xAB};
    auto r8 = FindValue(m.data(), m.size(), 0, 1, n8, 1, 10);
    CHECK_EQ(r8.Addrs.size(), size_t(2));
    CHECK_EQ(r8.Addrs[1], 12u);
}

TEST(search_bytes_any_offset_overlapping)
{
    auto m = Mem(32);
    Put(m, 3, "aaaa");
    auto r = FindValue(m.data(), m.size(), 0x100, 0, (const uint8_t*)"aa", 2, 10);
    CHECK_EQ(r.Addrs.size(), size_t(3));
    CHECK_EQ(r.Addrs[0], 0x103u);
    CHECK_EQ(r.Addrs[2], 0x105u);
}

TEST(search_limit_and_truncated)
{
    auto m = Mem(64);
    for (int i = 0; i < 8; i++)
        m[i * 8] = 7;
    uint8_t n[1] = {7};
    auto r = FindValue(m.data(), m.size(), 0, 1, n, 1, 3);
    CHECK_EQ(r.Addrs.size(), size_t(3));
    CHECK(r.Truncated);
    auto all = FindValue(m.data(), m.size(), 0, 1, n, 1, 8);
    CHECK_EQ(all.Addrs.size(), size_t(8));
    CHECK(!all.Truncated);
}

TEST(search_needle_longer_than_data_and_edges)
{
    auto m = Mem(4);
    auto r = FindValue(m.data(), m.size(), 0, 0, (const uint8_t*)"abcde", 5, 10);
    CHECK(r.Addrs.empty());
    Put(m, 2, "ab");
    auto e = FindValue(m.data(), m.size(), 0, 0, (const uint8_t*)"ab", 2, 10);
    CHECK_EQ(e.Addrs.size(), size_t(1));
    CHECK_EQ(e.Addrs[0], 2u);
    uint8_t n4[4] = {0, 0, 0, 0};
    auto z = FindValue(m.data(), 2, 0, 4, n4, 4, 10);
    CHECK(z.Addrs.empty());
}

TEST(encode_text_ascii_text)
{
    std::vector<EncodedText> out;
    CHECK(EncodeText("Hi", Protocol::EncAll, out));
    CHECK_EQ(out.size(), size_t(2));                    // UTF-8 equals ASCII, so it is left out
    CHECK_EQ(out[0].Encoding, Protocol::EncAscii);
    CHECK_EQ(out[1].Encoding, Protocol::EncUtf16le);
    CHECK_EQ(out[1].Bytes.size(), size_t(4));
    CHECK_EQ(out[1].Bytes[0], 'H');
    CHECK_EQ(out[1].Bytes[1], 0);
    out.clear();
    CHECK(EncodeText("Hi", Protocol::EncUtf8, out));
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0].Encoding, Protocol::EncUtf8);
}

TEST(encode_text_non_ascii)
{
    std::vector<EncodedText> out;
    CHECK(EncodeText("a\xC3\xA9", Protocol::EncAll, out));      // a, e-acute
    CHECK_EQ(out.size(), size_t(2));
    CHECK_EQ(out[0].Encoding, Protocol::EncUtf16le);
    CHECK_EQ(out[1].Encoding, Protocol::EncUtf8);
    CHECK_EQ(out[0].Bytes.size(), size_t(4));
    CHECK_EQ(out[0].Bytes[2], 0xE9);
    CHECK_EQ(out[1].Bytes.size(), size_t(3));
    out.clear();
    CHECK(EncodeText("\xC3\xA9", Protocol::EncAscii, out));
    CHECK(out.empty());
}

TEST(encode_text_surrogates_and_invalid)
{
    std::vector<EncodedText> out;
    CHECK(EncodeText("\xF0\x9F\x98\x80", Protocol::EncUtf16le, out));   // U+1F600
    CHECK_EQ(out.size(), size_t(1));
    const uint8_t expect[4] = {0x3D, 0xD8, 0x00, 0xDE};
    CHECK_EQ(out[0].Bytes.size(), size_t(4));
    for (int i = 0; i < 4; i++)
        CHECK_EQ(out[0].Bytes[i], expect[i]);
    out.clear();
    CHECK(!EncodeText("\xC3", Protocol::EncAll, out));
    CHECK(!EncodeText("\xFF\xFF", Protocol::EncAll, out));
    CHECK(!EncodeText("\xC0\x80", Protocol::EncAll, out));              // overlong
}

TEST(find_text_orders_by_address_then_encoding)
{
    auto m = Mem(128);
    Put(m, 40, "Hi");
    Put(m, 8, std::string("H\0i\0", 4));
    Put(m, 100, std::string("H\0i\0", 4));
    std::vector<EncodedText> pats;
    CHECK(EncodeText("Hi", Protocol::EncAll, pats));
    auto r = FindText(m.data(), m.size(), 0x1000, pats, 10);
    CHECK_EQ(r.Hits.size(), size_t(3));
    CHECK_EQ(r.Hits[0].Addr, 0x1008u);
    CHECK_EQ(r.Hits[0].Encoding, Protocol::EncUtf16le);
    CHECK_EQ(r.Hits[1].Addr, 0x1028u);
    CHECK_EQ(r.Hits[1].Encoding, Protocol::EncAscii);
    CHECK_EQ(r.Hits[2].Addr, 0x1064u);
    CHECK(!r.Truncated);
}

TEST(find_text_limit_truncates_after_merge)
{
    auto m = Mem(128);
    Put(m, 50, "Hi");
    Put(m, 60, "Hi");
    Put(m, 8, std::string("H\0i\0", 4));
    std::vector<EncodedText> pats;
    CHECK(EncodeText("Hi", Protocol::EncAll, pats));
    auto r = FindText(m.data(), m.size(), 0, pats, 2);
    CHECK_EQ(r.Hits.size(), size_t(2));
    CHECK_EQ(r.Hits[0].Addr, 8u);
    CHECK_EQ(r.Hits[1].Addr, 50u);
    CHECK(r.Truncated);
}

TEST(find_text_same_address_two_encodings)
{
    // "a\0" is UTF-16LE for "a" and also "a" followed by a NUL byte: both report
    auto m = Mem(16);
    Put(m, 4, "a");
    std::vector<EncodedText> pats;
    CHECK(EncodeText("a", Protocol::EncAll, pats));
    auto r = FindText(m.data(), m.size(), 0, pats, 10);
    CHECK_EQ(r.Hits.size(), size_t(2));
    CHECK_EQ(r.Hits[0].Addr, 4u);
    CHECK_EQ(r.Hits[0].Encoding, Protocol::EncAscii);
    CHECK_EQ(r.Hits[1].Addr, 4u);
    CHECK_EQ(r.Hits[1].Encoding, Protocol::EncUtf16le);
}
