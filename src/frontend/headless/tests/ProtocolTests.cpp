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

#include <cstdint>
#include <cstring>
#include <vector>

#include "TestHarness.h"
#include "Protocol.h"

using namespace Headless::Protocol;

typedef std::vector<uint8_t> Bytes;

TEST(proto_header_golden_bytes)
{
    Header h;
    h.Length = 0x01020304;
    h.Type = 0x0506;
    h.Console = 0x0708;
    uint8_t out[HeaderSize];
    EncodeHeader(h, out);
    const uint8_t golden[8] = {0x04, 0x03, 0x02, 0x01, 0x06, 0x05, 0x08, 0x07};
    CHECK(std::memcmp(out, golden, 8) == 0);

    Header d;
    DecodeHeader(golden, d);
    CHECK_EQ(d.Length, 0x01020304u);
    CHECK_EQ(d.Type, 0x0506);
    CHECK_EQ(d.Console, 0x0708);
}

TEST(proto_pack_golden_message)
{
    OkReply ok;
    ok.Seq = 0xAABBCCDD;
    Bytes m = Pack(3, ok);
    const uint8_t golden[] = {4, 0, 0, 0, 0x01, 0x80, 3, 0, 0xDD, 0xCC, 0xBB, 0xAA};
    CHECK_EQ(m.size(), sizeof golden);
    CHECK(m.size() == sizeof golden && std::memcmp(m.data(), golden, sizeof golden) == 0);
    CHECK_EQ(uint16_t(MsgType::ok), 0x8001);
}

TEST(proto_writer_reader_primitives)
{
    Writer w;
    w.U8(0x11);
    w.U16(0x2233);
    w.U32(0x44556677);
    w.U64(0x8899AABBCCDDEEFFull);
    w.String("hi");
    const uint8_t golden[] = {0x11, 0x33, 0x22, 0x77, 0x66, 0x55, 0x44,
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 2, 0, 0, 0, 'h', 'i'};
    CHECK_EQ(w.Buf.size(), sizeof golden);
    CHECK(w.Buf.size() == sizeof golden && std::memcmp(w.Buf.data(), golden, sizeof golden) == 0);

    Reader r(w.Buf.data(), w.Buf.size());
    CHECK_EQ(r.U8(), 0x11);
    CHECK_EQ(r.U16(), 0x2233);
    CHECK_EQ(r.U32(), 0x44556677u);
    CHECK(r.U64() == 0x8899AABBCCDDEEFFull);
    CHECK(r.String() == "hi");
    CHECK(r.Ok());
    CHECK(r.AtEnd());
}

TEST(proto_reader_underrun)
{
    const uint8_t data[3] = {1, 2, 3};
    Reader r(data, 3);
    CHECK_EQ(r.U16(), 0x0201);
    CHECK(r.Ok());
    CHECK_EQ(r.U32(), 0u); // underrun returns 0 and latches failure
    CHECK(!r.Ok());
    CHECK_EQ(r.U8(), 0); // stays failed even though a byte remains
    CHECK(!r.Ok());

    // A string whose length prefix exceeds the data
    const uint8_t bad[] = {100, 0, 0, 0, 'a'};
    Reader r2(bad, sizeof bad);
    CHECK(r2.String().empty());
    CHECK(!r2.Ok());
}

TEST(proto_decode_rejects_trailing_and_short)
{
    SetOption s;
    s.Seq = 7; s.Key = 2; s.Value = 9;
    Bytes b = Encode(s);
    SetOption d;
    CHECK(Decode(b, d));
    Bytes longer = b;
    longer.push_back(0);
    CHECK(!Decode(longer, d));
    Bytes shorter(b.begin(), b.end() - 1);
    CHECK(!Decode(shorter, d));
}

TEST(proto_parser_split_at_every_byte)
{
    Bytes a = BuildMessage(MsgType::read, 1, Bytes{1, 2, 3});
    Bytes b = BuildMessage(MsgType::pause, 0xFFFF, Bytes{});
    Bytes all = a;
    all.insert(all.end(), b.begin(), b.end());

    for (size_t split = 0; split <= all.size(); split++)
    {
        FrameParser p;
        CHECK(p.Feed(all.data(), split));
        CHECK(p.Feed(all.data() + split, all.size() - split));
        Message m;
        CHECK(p.Next(m));
        CHECK_EQ(m.Head.Type, uint16_t(MsgType::read));
        CHECK_EQ(m.Head.Console, 1);
        CHECK(m.Body == (Bytes{1, 2, 3}));
        CHECK(p.Next(m));
        CHECK_EQ(m.Head.Type, uint16_t(MsgType::pause));
        CHECK_EQ(m.Head.Console, 0xFFFF);
        CHECK(m.Body.empty());
        CHECK(!p.Next(m));
        CHECK(!p.Failed());
    }
}

TEST(proto_parser_byte_at_a_time_and_batches)
{
    Bytes all;
    for (int i = 0; i < 5; i++)
    {
        Bytes m = BuildMessage(MsgType::input, uint16_t(i), Bytes(size_t(i), uint8_t(i)));
        all.insert(all.end(), m.begin(), m.end());
    }
    FrameParser one;
    int got = 0;
    for (uint8_t c : all)
    {
        one.Feed(&c, 1);
        Message m;
        while (one.Next(m))
        {
            CHECK_EQ(m.Head.Console, got);
            CHECK_EQ(m.Body.size(), size_t(got));
            got++;
        }
    }
    CHECK_EQ(got, 5);

    FrameParser many;
    many.Feed(all.data(), all.size());
    Message m;
    int n = 0;
    while (many.Next(m))
        n++;
    CHECK_EQ(n, 5);
}

TEST(proto_parser_oversize)
{
    Header h;
    h.Length = MaxBody + 1;
    h.Type = uint16_t(MsgType::read);
    h.Console = 0;
    uint8_t raw[HeaderSize];
    EncodeHeader(h, raw);
    FrameParser p;
    CHECK(!p.Feed(raw, HeaderSize));
    CHECK(p.Failed());
    Message m;
    CHECK(!p.Next(m));

    // Exactly MaxBody is accepted
    h.Length = MaxBody;
    EncodeHeader(h, raw);
    FrameParser q;
    CHECK(q.Feed(raw, HeaderSize));
    CHECK(!q.Failed());
}

TEST(proto_hello_roundtrip)
{
    Hello h;
    h.Seq = 1; h.ProtocolVersion = 1; h.DefaultConsole = 0xFFFF; h.EmptyQueue = 2; h.StallTimeoutFrames = 15;
    Bytes b = Encode(h);
    CHECK_EQ(b.size(), size_t(4 + 4 + 2 + 1 + 1 + 2));
    Hello d;
    CHECK(Decode(b, d));
    CHECK_EQ(d.Seq, 1u);
    CHECK_EQ(d.ProtocolVersion, 1u);
    CHECK_EQ(d.DefaultConsole, 0xFFFF);
    CHECK_EQ(d.EmptyQueue, 2);
    CHECK_EQ(d.StallTimeoutFrames, 15);

    HelloReply r;
    r.Seq = 1; r.ProtocolVersion = 1; r.Name = "p2"; r.Pid = 1234; r.Port = 7778; r.DefaultConsole = 0;
    r.Ids = {0, 1, 5};
    HelloReply rd;
    CHECK(Decode(Encode(r), rd));
    CHECK(rd.Name == "p2");
    CHECK_EQ(rd.Pid, 1234u);
    CHECK_EQ(rd.Port, 7778);
    CHECK_EQ(rd.Ids.size(), size_t(3));
    CHECK(rd.Ids == (std::vector<uint16_t>{0, 1, 5}));
}

TEST(proto_set_option_roundtrip)
{
    SetOption s;
    s.Seq = 5; s.Key = OptDefaultConsole; s.Value = 3;
    Bytes b = Encode(s);
    CHECK_EQ(b.size(), size_t(12));
    SetOption d;
    CHECK(Decode(b, d));
    CHECK_EQ(d.Key, 3);
    CHECK_EQ(d.Value, 3u);
    CHECK_EQ(OptEmptyQueue, 1);
    CHECK_EQ(OptStallTimeoutFrames, 2);
}

static InputPayload MakePayload()
{
    InputPayload p;
    p.Seq = 99; p.Buttons = 0x0A5; p.Lid = 1;
    p.MicKind = 1;
    p.Touch.push_back({0, 10, 20, 1});
    p.Touch.push_back({1000, 255, 191, 0});
    p.Mic = {-1, 0, 32767, -32768};
    return p;
}

TEST(proto_input_payload_roundtrip_and_layout)
{
    InputPayload p = MakePayload();
    Bytes b = Encode(p);
    // 14 header bytes + 2 touch records of 12 + 4 samples of 2
    CHECK_EQ(b.size(), size_t(14 + 24 + 8));
    CHECK_EQ(b[0], 99);
    CHECK_EQ(b[4], 0xA5); // buttons little endian
    CHECK_EQ(b[5], 0x00);
    CHECK_EQ(b[6], 1);    // lid
    CHECK_EQ(b[7], 2);    // touchCount
    CHECK_EQ(b[8], 1);    // micKind
    CHECK_EQ(b[10], 4);   // micCount
    // second touch: offset 1000 at 14+12
    CHECK_EQ(b[26], 0xE8);
    CHECK_EQ(b[27], 0x03);
    CHECK_EQ(b[30], 255); // x
    CHECK_EQ(b[32], 191); // y
    CHECK_EQ(b[34], 0);   // down
    // mic sample -32768 last
    CHECK_EQ(b[b.size() - 2], 0x00);
    CHECK_EQ(b[b.size() - 1], 0x80);

    InputPayload d;
    CHECK(Decode(b, d));
    CHECK_EQ(d.Seq, 99u);
    CHECK_EQ(d.Buttons, 0x0A5);
    CHECK_EQ(d.Lid, 1);
    CHECK_EQ(d.Touch.size(), size_t(2));
    CHECK_EQ(d.Touch[1].Offset, 1000u);
    CHECK_EQ(d.Touch[1].X, 255);
    CHECK_EQ(d.Touch[1].Y, 191);
    CHECK_EQ(d.Mic.size(), size_t(4));
    CHECK_EQ(d.Mic[3], -32768);
    CHECK_EQ(d.Mic[0], -1);
    CHECK(ValidatePayload(d));
}

TEST(proto_input_payload_limits)
{
    InputPayload p = MakePayload();
    p.Touch.assign(MaxTouchPoints + 1, TouchPoint{0, 0, 0, 0});
    for (size_t i = 0; i < p.Touch.size(); i++)
        p.Touch[i].Offset = uint32_t(i);
    InputPayload d;
    CHECK(!Decode(Encode(p), d)); // touchCount > 16 is a bad payload

    InputPayload ord = MakePayload();
    ord.Touch[0].Offset = 500;
    ord.Touch[1].Offset = 500; // not strictly increasing
    CHECK(!ValidatePayload(ord));
    ord = MakePayload();
    ord.Touch[1].Offset = 560190; // past the frame
    CHECK(!ValidatePayload(ord));
    ord = MakePayload();
    ord.Touch[0].Y = 192;
    CHECK(!ValidatePayload(ord));
    ord = MakePayload();
    ord.Buttons = 0x1000;
    CHECK(!ValidatePayload(ord));
    ord = MakePayload();
    ord.Mic.assign(MaxMicSamples + 1, 0);
    CHECK(!ValidatePayload(ord));
}

TEST(proto_input_level_mic)
{
    InputPayload p;
    p.Seq = 1; p.MicKind = 2; p.MicLevel = 65535;
    InputPayload d;
    CHECK(Decode(Encode(p), d));
    CHECK_EQ(d.MicKind, 2);
    CHECK_EQ(d.MicLevel, 65535);
    CHECK(d.Mic.empty());
}

TEST(proto_input_multi_roundtrip)
{
    InputMulti m;
    m.Seq = 42;
    m.Entries.push_back({3, MakePayload()});
    InputPayload q;
    q.Seq = 43; q.Buttons = 1;
    m.Entries.push_back({0, q});
    InputMulti d;
    CHECK(Decode(Encode(m), d));
    CHECK_EQ(d.Seq, 42u);
    CHECK_EQ(d.Entries.size(), size_t(2));
    CHECK_EQ(d.Entries[0].Console, 3);
    CHECK_EQ(d.Entries[0].Payload.Touch.size(), size_t(2));
    CHECK_EQ(d.Entries[0].Payload.Mic.size(), size_t(4));
    CHECK_EQ(d.Entries[1].Console, 0);
    CHECK_EQ(d.Entries[1].Payload.Seq, 43u);
    CHECK_EQ(d.Entries[1].Payload.Buttons, 1);
}

TEST(proto_subscribe_roundtrip)
{
    Subscribe s;
    s.Seq = 8; s.Id = 77; s.EveryNFrames = 2;
    s.Ranges.push_back({0, 0x02000000, 16});
    s.Ranges.push_back({1, 0x03800000, 4});
    Bytes b = Encode(s);
    CHECK_EQ(b.size(), size_t(4 + 4 + 4 + 2 + 2 * 9));
    Subscribe d;
    CHECK(Decode(b, d));
    CHECK_EQ(d.Id, 77u);
    CHECK_EQ(d.EveryNFrames, 2u);
    CHECK_EQ(d.Ranges.size(), size_t(2));
    CHECK_EQ(d.Ranges[1].Cpu, 1);
    CHECK_EQ(d.Ranges[1].Addr, 0x03800000u);
    CHECK_EQ(d.Ranges[1].Len, 4u);
}

TEST(proto_frame_event_roundtrip)
{
    FrameEvent e;
    e.Frame = 0x1122334455ull; e.AppliedSeq = 9; e.InputApplied = 1; e.WaitingForInput = 0;
    e.QueueDepth = 3; e.Dropped = 2;
    e.Blocks.push_back({5, {{1, 2, 3}, {4}}});
    e.Blocks.push_back({6, {}});
    Bytes b = Encode(e);
    CHECK_EQ(b[0], 0); // events carry seq 0
    CHECK_EQ(b[3], 0);
    FrameEvent d;
    CHECK(Decode(b, d));
    CHECK_EQ(d.Seq, 0u);
    CHECK(d.Frame == 0x1122334455ull);
    CHECK_EQ(d.AppliedSeq, 9u);
    CHECK_EQ(d.InputApplied, 1);
    CHECK_EQ(d.QueueDepth, 3);
    CHECK_EQ(d.Dropped, 2u);
    CHECK_EQ(d.Blocks.size(), size_t(2));
    CHECK_EQ(d.Blocks[0].SubId, 5u);
    CHECK_EQ(d.Blocks[0].Ranges.size(), size_t(2));
    CHECK(d.Blocks[0].Ranges[0] == (Bytes{1, 2, 3}));
    CHECK(d.Blocks[0].Ranges[1] == (Bytes{4}));
    CHECK(d.Blocks[1].Ranges.empty());
}

TEST(proto_break_event_roundtrip)
{
    BreakEvent e;
    e.Kind = 2; e.Cpu = 1; e.Size = 4; e.Id = -1; e.Pc = 0x02000100; e.Addr = 0x02001000; e.Value = 0xDEADBEEF; e.Dma = -1;
    BreakEvent d;
    CHECK(Decode(Encode(e), d));
    CHECK_EQ(d.Id, -1);
    CHECK_EQ(d.Dma, -1);
    CHECK_EQ(d.Value, 0xDEADBEEFu);
    CHECK_EQ(d.Pc, 0x02000100u);
}

TEST(proto_memory_search_roundtrip)
{
    MemorySearch s;
    s.Seq = 3; s.Cpu = 0; s.Start = 0x02000000; s.Length = 0x1000; s.Size = 4; s.Limit = 10;
    s.Needle = {0x78, 0x56, 0x34, 0x12};
    MemorySearch d;
    CHECK(Decode(Encode(s), d));
    CHECK_EQ(d.Size, 4);
    CHECK(d.Needle == s.Needle);
    CHECK_EQ(d.Limit, 10u);
    CHECK(ValidateSearch(d));

    d.Size = 2; // needle length does not match size
    CHECK(!ValidateSearch(d));
    d.Size = 3;
    CHECK(!ValidateSearch(d));
    d.Size = 0;
    d.Needle.clear();
    CHECK(!ValidateSearch(d));
    d.Needle = {1, 2, 3};
    CHECK(ValidateSearch(d));

    MemorySearchReply r;
    r.Seq = 3; r.Truncated = 1; r.Addrs = {0x02000010, 0x02000020};
    MemorySearchReply rd;
    CHECK(Decode(Encode(r), rd));
    CHECK_EQ(rd.Truncated, 1);
    CHECK(rd.Addrs == r.Addrs);
}

TEST(proto_search_defaults)
{
    CHECK_EQ(ResolveStart(0, 0), DefaultSearchStart);
    CHECK_EQ(ResolveLength(0, 0), DefaultSearchLength);
    CHECK_EQ(ResolveStart(0x03000000, 0x100), 0x03000000u);
    CHECK_EQ(ResolveLength(0x03000000, 0x100), 0x100u);
    CHECK_EQ(DefaultSearchStart, 0x02000000u);
    CHECK_EQ(DefaultSearchLength, 0x400000u);
    CHECK_EQ(ResolveSearchLimit(0, DefaultMemorySearchLimit), 64u);
    CHECK_EQ(ResolveSearchLimit(0, DefaultTextFindLimit), 32u);
    CHECK_EQ(ResolveSearchLimit(100000, DefaultTextFindLimit), 4096u);
    CHECK_EQ(ResolveSearchLimit(5, DefaultTextFindLimit), 5u);
    CHECK_EQ(ResolveEncodings(0), 7);
    CHECK_EQ(ResolveEncodings(EncUtf16le), 2);
    CHECK_EQ(EncAscii, 1);
    CHECK_EQ(EncUtf8, 4);
}

TEST(proto_text_find_roundtrip)
{
    TextFind t;
    t.Seq = 4; t.Cpu = 0; t.Start = 0x02000000; t.Length = 0x400000; t.Text = "Slime \xE3\x82\xB9"; t.Encodings = 6; t.Limit = 32;
    TextFind d;
    CHECK(Decode(Encode(t), d));
    CHECK(d.Text == t.Text);
    CHECK_EQ(d.Encodings, 6);
    CHECK_EQ(d.Limit, 32u);

    TextFindReply r;
    r.Seq = 4; r.Truncated = 0;
    r.Hits.push_back({0x02000100, EncAscii});
    r.Hits.push_back({0x02000200, EncUtf16le});
    TextFindReply rd;
    CHECK(Decode(Encode(r), rd));
    CHECK_EQ(rd.Hits.size(), size_t(2));
    CHECK_EQ(rd.Hits[1].Addr, 0x02000200u);
    CHECK_EQ(rd.Hits[1].Encoding, EncUtf16le);
}

TEST(proto_error_reply_roundtrip)
{
    ErrorReply e;
    e.Seq = 11; e.Code = ErrorCode::input_queue_full; e.Text = "queue full (600)";
    Bytes m = Pack(2, e);
    FrameParser p;
    p.Feed(m.data(), m.size());
    Message msg;
    CHECK(p.Next(msg));
    CHECK_EQ(msg.Head.Type, uint16_t(MsgType::error));
    CHECK_EQ(msg.Head.Console, 2);
    ErrorReply d;
    CHECK(Decode(msg.Body, d));
    CHECK_EQ(d.Seq, 11u);
    CHECK(d.Code == ErrorCode::input_queue_full);
    CHECK(d.Text == "queue full (600)");
}

TEST(proto_message_numbers_are_stable)
{
    CHECK_EQ(uint16_t(MsgType::hello), 0x0001);
    CHECK_EQ(uint16_t(MsgType::console_create), 0x0101);
    CHECK_EQ(uint16_t(MsgType::run), 0x0201);
    CHECK_EQ(uint16_t(MsgType::input), 0x0301);
    CHECK_EQ(uint16_t(MsgType::read), 0x0401);
    CHECK_EQ(uint16_t(MsgType::memory_search), 0x0501);
    CHECK_EQ(uint16_t(MsgType::subscribe), 0x0601);
    CHECK_EQ(uint16_t(MsgType::breakpoint_add), 0x0701);
    CHECK_EQ(uint16_t(MsgType::pacing), 0x0801);
    CHECK_EQ(uint16_t(MsgType::ok), 0x8001);
    CHECK_EQ(uint16_t(MsgType::error), 0x8002);
    CHECK_EQ(uint16_t(MsgType::data), 0x8003);
    CHECK_EQ(uint16_t(MsgType::frame), 0x9001);
    CHECK(IsKnownType(uint16_t(MsgType::packets_read)));
    CHECK(!IsKnownType(0x7777));
}

TEST(proto_analysis_roundtrips)
{
    PointListReply pl;
    pl.Seq = 1;
    pl.Points.push_back({-3, 1, 1, 1, 3, 0x02000000, 0x02000003, 0x123456789ull});
    PointListReply pld;
    CHECK(Decode(Encode(pl), pld));
    CHECK_EQ(pld.Points.size(), size_t(1));
    CHECK_EQ(pld.Points[0].Id, -3);
    CHECK_EQ(pld.Points[0].Hits, 0x123456789ull);
    CHECK_EQ(pld.Points[0].Enabled, 1);

    TraceReadReply tr;
    tr.Seq = 2; tr.Truncated = 1;
    tr.Entries.push_back({0x1122334455667788ull, 1, 0x02000000, 0xE12FFF1E, 0x60000010});
    TraceReadReply trd;
    CHECK(Decode(Encode(tr), trd));
    CHECK_EQ(trd.Truncated, 1);
    CHECK(trd.Entries[0].Cycle == 0x1122334455667788ull);
    CHECK_EQ(trd.Entries[0].Cpsr, 0x60000010u);
    CHECK_EQ(Encode(tr).size(), size_t(4 + 1 + 4 + 21));

    CallStackReply cs;
    cs.Seq = 3;
    cs.Frames.push_back({0x100, 0x200, 0x104, 0x03000000, 0x1F, 1});
    CallStackReply csd;
    CHECK(Decode(Encode(cs), csd));
    CHECK_EQ(csd.Frames[0].Entry, 0x200u);
    CHECK_EQ(csd.Frames[0].Exception, 1);

    CallCountsReply cc;
    cc.Seq = 4; cc.Truncated = 0;
    cc.Counts.push_back({0x02000000, 1ull << 40});
    CallCountsReply ccd;
    CHECK(Decode(Encode(cc), ccd));
    CHECK(ccd.Counts[0].Count == (1ull << 40));

    LastWriterReply lw;
    lw.Seq = 5; lw.Kind = 2; lw.Cpu = 1; lw.Channel = 3; lw.Pc = 0; lw.Raw = 0xABCD;
    LastWriterReply lwd;
    CHECK(Decode(Encode(lw), lwd));
    CHECK_EQ(lwd.Kind, 2);
    CHECK_EQ(lwd.Channel, 3);

    PacketsReadReply pk;
    pk.Seq = 6;
    pk.Packets.push_back({123456789ull, 1, 2, {9, 8, 7}});
    PacketsReadReply pkd;
    CHECK(Decode(Encode(pk), pkd));
    CHECK(pkd.Packets[0].Data == (Bytes{9, 8, 7}));
    CHECK_EQ(pkd.Packets[0].Type, 2);

    CoverageReadReply cv;
    cv.Seq = 7; cv.Truncated = 1; cv.Addrs = {1, 2, 3};
    CoverageReadReply cvd;
    CHECK(Decode(Encode(cv), cvd));
    CHECK(cvd.Addrs == cv.Addrs);
}

TEST(proto_with_symbols_flag_is_optional_and_trailing)
{
    // An older client's request has no flag: it decodes with WithSymbols = 0.
    Writer w;
    w.U32(9); w.U8(9); w.U32(100);                 // coverage_read: seq, cpu, max
    CoverageRead cr;
    CHECK(Decode(w.Buf, cr));
    CHECK_EQ(cr.WithSymbols, 0);
    CHECK_EQ(cr.Max, 100u);
    w.U8(1);
    CHECK(Decode(w.Buf, cr));
    CHECK_EQ(cr.WithSymbols, 1);
    // Our own encoding always carries it.
    CHECK_EQ(Encode(cr).size(), size_t(4 + 1 + 4 + 1));

    TraceRead tr; tr.Seq = 1; tr.Max = 7; tr.WithSymbols = 1;
    TraceRead trd;
    CHECK(Decode(Encode(tr), trd));
    CHECK_EQ(trd.WithSymbols, 1);

    MemorySearch ms; ms.Seq = 2; ms.Size = 4; ms.Needle = {1, 2, 3, 4}; ms.WithSymbols = 1;
    MemorySearch msd;
    CHECK(Decode(Encode(ms), msd));
    CHECK_EQ(msd.WithSymbols, 1);
    TextFind tf; tf.Seq = 3; tf.Text = "abc"; tf.WithSymbols = 1;
    TextFind tfd;
    CHECK(Decode(Encode(tf), tfd));
    CHECK_EQ(tfd.WithSymbols, 1);
    CallStackReq cs; cs.Seq = 4; cs.Cpu = 7; cs.WithSymbols = 1;
    CallStackReq csd;
    CHECK(Decode(Encode(cs), csd));
    CHECK_EQ(csd.WithSymbols, 1);
    CallCountsRead cc; cc.Seq = 5; cc.Cpu = 9; cc.Max = 3; cc.WithSymbols = 1;
    CallCountsRead ccd;
    CHECK(Decode(Encode(cc), ccd));
    CHECK_EQ(ccd.Max, 3u);
}

TEST(proto_symbol_names_section)
{
    // Without names the reply keeps its old layout; with them a names section follows the list.
    CoverageReadReply plain;
    plain.Seq = 1; plain.Addrs = {0x02000000, 0x02000004};
    CHECK_EQ(Encode(plain).size(), size_t(4 + 1 + 4 + 8));
    CoverageReadReply plainDecoded;
    CHECK(Decode(Encode(plain), plainDecoded));
    CHECK_EQ(plainDecoded.HasNames, 0);

    CoverageReadReply named = plain;
    named.HasNames = 1;
    named.Names = {"main", ""};
    CoverageReadReply namedDecoded;
    CHECK(Decode(Encode(named), namedDecoded));
    CHECK_EQ(namedDecoded.HasNames, 1);
    CHECK(namedDecoded.Names == named.Names);

    CallStackReply stack;
    stack.Seq = 2;
    stack.Frames.push_back({0x100, 0x200, 0x104, 0x03000000, 0x1F, 0});
    stack.HasNames = 1;
    stack.Names = {"callee"};
    stack.SiteNames = {"caller+0x4"};
    CallStackReply stackDecoded;
    CHECK(Decode(Encode(stack), stackDecoded));
    CHECK(stackDecoded.Names == stack.Names);
    CHECK(stackDecoded.SiteNames == stack.SiteNames);

    TraceReadReply trace;
    trace.Seq = 3;
    trace.Entries.push_back({1, 9, 0x02000000, 0xE1A00000, 0x60000010});
    trace.HasNames = 1;
    trace.Names = {"start"};
    TraceReadReply traceDecoded;
    CHECK(Decode(Encode(trace), traceDecoded));
    CHECK(traceDecoded.Names == trace.Names);
}

TEST(proto_symbols_load_and_read_max)
{
    CHECK_EQ(uint16_t(MsgType::symbols_load), 0x0716);
    CHECK(IsKnownType(0x0716));
    SymbolsLoad sl; sl.Seq = 1; sl.Text = "game.sym";
    SymbolsLoad sld;
    CHECK(Decode(Encode(sl), sld));
    CHECK(sld.Text == "game.sym");
    SymbolsLoadReply r; r.Seq = 1; r.Symbols = 12; r.Modules = 3;
    SymbolsLoadReply rd;
    CHECK(Decode(Encode(r), rd));
    CHECK_EQ(rd.Symbols, 12u);
    CHECK_EQ(ResolveReadMax(0), DefaultReadMax);
    CHECK_EQ(ResolveReadMax(5), 5u);
    CHECK_EQ(ResolveReadMax(1u << 30), MaxReadMax);
}

TEST(proto_request_roundtrips)
{
    Run r; r.Seq = 1; r.Frames = 60; r.Consoles = {0, 2};
    Run rd;
    CHECK(Decode(Encode(r), rd));
    CHECK_EQ(rd.Frames, 60u);
    CHECK(rd.Consoles == r.Consoles);

    LiveStart ls; ls.Seq = 2; ls.Fps = 60;
    LiveStart lsd;
    CHECK(Decode(Encode(ls), lsd));
    CHECK(lsd.Consoles.empty());

    Step st; st.Seq = 3; st.Cpu = 1; st.Count = 5;
    Step std_;
    CHECK(Decode(Encode(st), std_));
    CHECK_EQ(std_.Cpu, 1);
    CHECK_EQ(std_.Count, 5u);

    ConsoleCreate cc; cc.Seq = 4; cc.Bios9 = "a"; cc.Bios7 = "b"; cc.Firmware = ""; cc.Nickname = "nick"; cc.Debug = 1;
    ConsoleCreate ccd;
    CHECK(Decode(Encode(cc), ccd));
    CHECK(ccd.Bios7 == "b");
    CHECK(ccd.Firmware.empty());
    CHECK(ccd.Nickname == "nick");
    CHECK_EQ(ccd.Debug, 1);

    RomLoad rl; rl.Seq = 5; rl.Rom = "game.nds"; rl.Save = "";
    RomLoad rld;
    CHECK(Decode(Encode(rl), rld));
    CHECK(rld.Rom == "game.nds");

    ReadReq rq; rq.Seq = 6; rq.Cpu = 1; rq.Addr = 0x02000000; rq.Len = 16;
    ReadReq rqd;
    CHECK(Decode(Encode(rq), rqd));
    CHECK_EQ(rqd.Len, 16u);

    WriteReq wq; wq.Seq = 7; wq.Cpu = 0; wq.Addr = 0x02000000; wq.Data = {1, 2, 3};
    WriteReq wqd;
    CHECK(Decode(Encode(wq), wqd));
    CHECK(wqd.Data == wq.Data);

    ScreenshotReply sc; sc.Seq = 8; sc.Frame = 77; sc.Pixels.assign(ScreenshotBytes, 0xAB);
    ScreenshotReply scd;
    CHECK(Decode(Encode(sc), scd));
    CHECK_EQ(scd.Pixels.size(), size_t(393216));
    CHECK_EQ(scd.Frame, 77u);
    sc.Pixels.pop_back();
    CHECK(!Decode(Encode(sc), scd));

    RegistersReply rg; rg.Seq = 9;
    for (uint32_t i = 0; i < 19; i++) rg.Regs.push_back(i);
    RegistersReply rgd;
    CHECK(Decode(Encode(rg), rgd));
    CHECK_EQ(rgd.Regs.size(), size_t(19));
    CHECK_EQ(rgd.Regs[18], 18u);

    TraceConfig tc; tc.Seq = 10; tc.CpuMask = 3; tc.Start = 1; tc.End = 2; tc.Capacity = 1000;
    TraceConfig tcd;
    CHECK(Decode(Encode(tc), tcd));
    CHECK_EQ(tcd.Capacity, 1000u);
}

TEST(proto_resolve_console)
{
    int out = -2;
    std::vector<int> ids = {2, 0, 5};
    CHECK(ResolveConsole(DefaultConsole, -1, ids, out) == ErrorCode::none);
    CHECK_EQ(out, 0); // lowest existing id regardless of order
    CHECK(ResolveConsole(DefaultConsole, 5, ids, out) == ErrorCode::none);
    CHECK_EQ(out, 5); // pinned default
    CHECK(ResolveConsole(DefaultConsole, 3, ids, out) == ErrorCode::no_console); // pinned but gone: no fallback
    CHECK(ResolveConsole(2, 5, ids, out) == ErrorCode::none); // explicit beats pinned
    CHECK_EQ(out, 2);
    CHECK(ResolveConsole(7, -1, ids, out) == ErrorCode::no_console);
    std::vector<int> none;
    CHECK(ResolveConsole(DefaultConsole, -1, none, out) == ErrorCode::no_console);
    CHECK(ResolveConsole(0, -1, none, out) == ErrorCode::no_console);
    CHECK(ResolveConsole(DefaultConsole, 0, none, out) == ErrorCode::no_console);
}
