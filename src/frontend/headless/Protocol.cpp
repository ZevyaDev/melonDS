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

#include "Protocol.h"

#include <algorithm>

namespace Headless::Protocol
{

static const uint16_t KnownTypes[] =
{
    0x0001, 0x0002,
    0x0101, 0x0102, 0x0103, 0x0104, 0x0105, 0x0106,
    0x0201, 0x0202, 0x0203, 0x0204, 0x0205,
    0x0301, 0x0302, 0x0303,
    0x0401, 0x0402, 0x0403, 0x0404, 0x0405,
    0x0501, 0x0502,
    0x0601, 0x0602,
    0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708, 0x0709, 0x070A,
    0x070B, 0x070C, 0x070D, 0x070E, 0x070F, 0x0710, 0x0711, 0x0712, 0x0713, 0x0714, 0x0715,
    0x0801, 0x0802,
    0x8001, 0x8002, 0x8003,
    0x9001, 0x9002, 0x9003, 0x9004, 0x9005,
};

bool IsKnownType(uint16_t type)
{
    return std::find(std::begin(KnownTypes), std::end(KnownTypes), type) != std::end(KnownTypes);
}

const char* ErrorName(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::none: return "none";
    case ErrorCode::bad_message: return "bad_message";
    case ErrorCode::unknown_type: return "unknown_type";
    case ErrorCode::no_console: return "no_console";
    case ErrorCode::busy: return "busy";
    case ErrorCode::console_running: return "console_running";
    case ErrorCode::input_queue_full: return "input_queue_full";
    case ErrorCode::bad_payload: return "bad_payload";
    case ErrorCode::bad_option: return "bad_option";
    case ErrorCode::jit_on: return "jit_on";
    case ErrorCode::not_supported: return "not_supported";
    case ErrorCode::internal: return "internal";
    case ErrorCode::failed: return "failed";
    }
    return "unknown";
}

void EncodeHeader(const Header& h, uint8_t out[HeaderSize])
{
    Writer w;
    w.U32(h.Length);
    w.U16(h.Type);
    w.U16(h.Console);
    std::memcpy(out, w.Buf.data(), HeaderSize);
}

void DecodeHeader(const uint8_t in[HeaderSize], Header& h)
{
    Reader r(in, HeaderSize);
    h.Length = r.U32();
    h.Type = r.U16();
    h.Console = r.U16();
}

std::vector<uint8_t> BuildMessage(MsgType type, uint16_t console, const std::vector<uint8_t>& body)
{
    Header h;
    h.Length = uint32_t(body.size());
    h.Type = uint16_t(type);
    h.Console = console;
    std::vector<uint8_t> out(HeaderSize);
    EncodeHeader(h, out.data());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool FrameParser::CheckHead()
{
    if (Buf.size() - Pos >= HeaderSize)
    {
        Header h;
        DecodeHeader(Buf.data() + Pos, h);
        if (h.Length > MaxBody)
            Bad = true;
    }
    return !Bad;
}

bool FrameParser::Feed(const uint8_t* data, size_t len)
{
    if (Bad)
        return false;
    Buf.insert(Buf.end(), data, data + len);
    return CheckHead();
}

bool FrameParser::Next(Message& out)
{
    if (Bad || !CheckHead())
        return false;
    size_t avail = Buf.size() - Pos;
    if (avail < HeaderSize)
        return false;
    DecodeHeader(Buf.data() + Pos, out.Head);
    if (avail < HeaderSize + out.Head.Length)
        return false;
    const uint8_t* body = Buf.data() + Pos + HeaderSize;
    out.Body.assign(body, body + out.Head.Length);
    Pos += HeaderSize + out.Head.Length;
    if (Pos == Buf.size())
    {
        Buf.clear();
        Pos = 0;
    }
    else if (Pos > (1u << 16))
    {
        Buf.erase(Buf.begin(), Buf.begin() + Pos);
        Pos = 0;
    }
    return true;
}

ErrorCode ResolveConsole(uint16_t headerConsole, int pinnedDefault, const std::vector<int>& existingIds, int& out)
{
    auto exists = [&](int id) {
        return std::find(existingIds.begin(), existingIds.end(), id) != existingIds.end();
    };
    if (headerConsole == DefaultConsole)
    {
        if (pinnedDefault >= 0)
        {
            if (!exists(pinnedDefault))
                return ErrorCode::no_console;
            out = pinnedDefault;
            return ErrorCode::none;
        }
        if (existingIds.empty())
            return ErrorCode::no_console;
        out = *std::min_element(existingIds.begin(), existingIds.end());
        return ErrorCode::none;
    }
    if (!exists(headerConsole))
        return ErrorCode::no_console;
    out = headerConsole;
    return ErrorCode::none;
}

bool ValidatePayload(const InputPayload& p)
{
    if (p.Buttons >= 0x1000 || p.Lid > 1 || p.MicKind > 2)
        return false;
    if (p.Touch.size() > MaxTouchPoints)
        return false;
    if (!p.Mic.empty() && (p.MicKind != 1 || p.Mic.size() > MaxMicSamples))
        return false;
    for (size_t i = 0; i < p.Touch.size(); i++)
    {
        const TouchPoint& t = p.Touch[i];
        if (t.Offset >= FrameCycles || t.X > 255 || t.Y > 191 || t.Down > 1)
            return false;
        if (i && t.Offset <= p.Touch[i - 1].Offset)
            return false;
    }
    return true;
}

bool ValidateSearch(const MemorySearch& s)
{
    if (s.Size == 1 || s.Size == 2 || s.Size == 4)
        return s.Needle.size() == s.Size;
    return s.Size == 0 && !s.Needle.empty();
}

}
