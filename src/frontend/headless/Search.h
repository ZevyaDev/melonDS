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

#ifndef HEADLESS_SEARCH_H
#define HEADLESS_SEARCH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Headless::Search
{

struct ValueHits
{
    std::vector<uint32_t> Addrs;
    bool Truncated = false;
};

// Scan data[0, len), which is guest memory starting at address `base`.
// size 1/2/4: a little-endian value (needle holds `size` bytes) at addresses that are
// multiples of size. size 0: the byte string needle[0, needleLen) at any address.
// Matches may overlap. At most `limit` addresses are returned, ascending; Truncated
// says there were more.
ValueHits FindValue(const uint8_t* data, size_t len, uint32_t base, unsigned size,
                    const uint8_t* needle, size_t needleLen, uint32_t limit);

struct TextHit
{
    uint32_t Addr = 0;
    uint8_t Encoding = 0;   // Protocol::EncAscii, EncUtf16le or EncUtf8
};

struct TextHits
{
    std::vector<TextHit> Hits;
    bool Truncated = false;
};

struct EncodedText
{
    uint8_t Encoding;
    std::vector<uint8_t> Bytes;
};

// Encode UTF-8 text for the selected encodings (mask of Protocol::Enc*). ASCII is left out
// when the text has non-ASCII characters; UTF-8 is left out when it would be byte for byte
// the same as a selected ASCII encoding. Returns false if text is not valid UTF-8.
bool EncodeText(const std::string& utf8, uint8_t mask, std::vector<EncodedText>& out);

// Find every encoding of the text in data[0, len) (guest memory at `base`), at any address.
// Hits are ordered by address, then by encoding value.
TextHits FindText(const uint8_t* data, size_t len, uint32_t base, const std::vector<EncodedText>& patterns,
                  uint32_t limit);

}

#endif
