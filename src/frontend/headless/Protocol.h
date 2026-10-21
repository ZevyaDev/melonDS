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

#ifndef HEADLESS_PROTOCOL_H
#define HEADLESS_PROTOCOL_H

/*
    melonds_server wire protocol, version 1. This header is the specification;
    the Python client follows it field for field.

    Transport: one TCP connection, to 127.0.0.1 by default (mds_serve_host and melonds_server --host can
    listen on another IPv4 address). Everything is little-endian.
    Nothing relies on struct memory layout: every message is an explicit field
    list, written and read by Writer/Reader below. Types: u8/u16/u32/u64
    unsigned, i32 signed two's complement, string = u32 byte length + UTF-8
    (no terminator), bytes = u32 length + raw bytes.

    Framing: every message is an 8-byte header followed by the body.

        offset 0  u32 length   length of the BODY in bytes, NOT counting these
                               8 header bytes. Greater than MaxBody (16 MiB)
                               is a protocol error and the connection is closed.
        offset 4  u16 type     MsgType
        offset 6  u16 console  target console id, or 0xFFFF = default console.
                               In replies and events it is the resolved/source
                               console. Ignored by session/process messages.

    Bodies: every request body starts with u32 seq, chosen by the client.
    The reply (ok, error or data) echoes it as its first field. Events
    (frame, break_, lua_output, input_queue, console_gone) start with seq = 0.
    Every reply to a request is exactly one of ok / error / data; the data
    layout is fixed by the request type (see each struct below). A body must
    be consumed exactly: trailing bytes or underrun are bad_payload.

    Console resolution: see ResolveConsole.

    Input payload seq: the InputPayload's own seq field IS the request seq
    (one field). The server echoes it in the frame event that applies the
    payload (FrameEvent::AppliedSeq). For input_multi each entry's payload
    carries its own seq, free for the client to choose (the request seq goes
    in front of the entry list).

    Message numbering: MsgType = (group << 8) | index. Numbers are never
    reused or renumbered; new messages get the next free index in a group.
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace Headless::Protocol
{

constexpr uint32_t ProtocolVersion = 1;
constexpr size_t HeaderSize = 8;
constexpr uint32_t MaxBody = 16u * 1024 * 1024;
constexpr uint16_t DefaultConsole = 0xFFFF;

constexpr size_t ScreenshotBytes = 256 * 384 * 4; // 393216, BGRA, top screen above bottom
constexpr unsigned MaxTouchPoints = 16;
constexpr unsigned MaxMicSamples = 796;
constexpr uint32_t FrameCycles = 560190;          // touch offsets are < this
constexpr uint32_t MaxSubscribeRanges = 64;

// memory_search / text_find
constexpr uint32_t DefaultSearchStart = 0x02000000;
constexpr uint32_t DefaultSearchLength = 0x400000;
constexpr uint32_t DefaultMemorySearchLimit = 64;
constexpr uint32_t DefaultTextFindLimit = 32;
constexpr uint32_t MaxSearchLimit = 4096;
constexpr uint8_t EncAscii = 1;
constexpr uint8_t EncUtf16le = 2;
constexpr uint8_t EncUtf8 = 4;
constexpr uint8_t EncAll = 7;

// Clearing reads (trace_read, call_counts_read, coverage_read, packets_read, input_log_read): max 0 selects the
// default; above MaxReadMax is clamped. Replies are also cut short at about 8 MiB.
constexpr uint32_t DefaultReadMax = 4096;
constexpr uint32_t MaxReadMax = 65536;
constexpr size_t MaxReplyBytes = 8u * 1024 * 1024;
inline uint32_t ResolveReadMax(uint32_t max) { return max == 0 ? DefaultReadMax : (max > MaxReadMax ? MaxReadMax : max); }

// set_option keys
constexpr uint16_t OptEmptyQueue = 1;          // value 0 repeat, 1 neutral, 2 wait
constexpr uint16_t OptStallTimeoutFrames = 2;  // repeat mode; 0 = never
constexpr uint16_t OptDefaultConsole = 3;      // 0xFFFF = lowest existing id

enum class MsgType : uint16_t
{
    // 0x00xx session
    hello = 0x0001,
    set_option = 0x0002,

    // 0x01xx lifecycle
    console_create = 0x0101,
    console_destroy = 0x0102,
    console_list = 0x0103,
    rom_load = 0x0104,
    reset = 0x0105,
    shutdown = 0x0106,

    // 0x02xx execution
    run = 0x0201,
    live_start = 0x0202,
    live_stop = 0x0203,
    pause = 0x0204,
    step = 0x0205,

    // 0x03xx input
    input = 0x0301,
    input_multi = 0x0302,
    input_log_read = 0x0303,

    // 0x04xx memory
    read = 0x0401,
    write = 0x0402,
    registers = 0x0403,
    set_register = 0x0404,
    screenshot = 0x0405,

    // 0x05xx search
    memory_search = 0x0501,
    text_find = 0x0502,

    // 0x06xx subscriptions
    subscribe = 0x0601,
    unsubscribe = 0x0602,

    // 0x07xx debug and analysis
    breakpoint_add = 0x0701,
    watchpoint_add = 0x0702,
    point_remove = 0x0703,
    point_list = 0x0704,
    point_enable = 0x0705,
    savestate_save = 0x0706,
    savestate_load = 0x0707,
    lua_run = 0x0708,
    lua_exec = 0x0709,
    lua_stop = 0x070A,
    trace_config = 0x070B,
    trace_read = 0x070C,
    calls_track = 0x070D,
    callstack = 0x070E,
    call_counts_read = 0x070F,
    coverage_track = 0x0710,
    coverage_read = 0x0711,
    writers_track = 0x0712,
    last_writer = 0x0713,
    packets_track = 0x0714,
    packets_read = 0x0715,
    symbols_load = 0x0716,

    // 0x08xx process-wide
    pacing = 0x0801,
    mp_timeout = 0x0802,

    // 0x80xx replies (server -> client)
    ok = 0x8001,
    error = 0x8002,
    data = 0x8003,

    // 0x90xx events (server -> client)
    frame = 0x9001,
    break_ = 0x9002,
    lua_output = 0x9003,
    input_queue = 0x9004,
    console_gone = 0x9005,
};

bool IsKnownType(uint16_t type);

enum class ErrorCode : uint16_t
{
    none = 0,
    bad_message = 1,       // framing/header problem
    unknown_type = 2,
    no_console = 3,
    busy = 4,              // a second client tried to connect
    console_running = 5,   // message needs a stopped console
    input_queue_full = 6,
    bad_payload = 7,       // body does not decode or fails validation
    bad_option = 8,
    jit_on = 9,            // reserved: headless consoles never run the JIT, so the server never sends it
    not_supported = 10,
    internal = 11,
    failed = 12,           // the underlying call failed; the text says why
};

const char* ErrorName(ErrorCode code);

// ---------------------------------------------------------------------------
// Header

struct Header
{
    uint32_t Length = 0; // body length in bytes, excluding the 8 header bytes
    uint16_t Type = 0;
    uint16_t Console = DefaultConsole;
};

void EncodeHeader(const Header& h, uint8_t out[HeaderSize]);
void DecodeHeader(const uint8_t in[HeaderSize], Header& h);

struct Message
{
    Header Head;
    std::vector<uint8_t> Body;
};

// Header + body as one buffer ready to send.
std::vector<uint8_t> BuildMessage(MsgType type, uint16_t console, const std::vector<uint8_t>& body);

// Incremental framing: feed arbitrary chunks from recv, pull complete messages.
class FrameParser
{
public:
    // Returns false once the stream is bad (oversize length); stays false.
    bool Feed(const uint8_t* data, size_t len);
    // Pops the next complete message in arrival order.
    bool Next(Message& out);
    bool Failed() const { return Bad; }

private:
    bool CheckHead();

    std::vector<uint8_t> Buf;
    size_t Pos = 0;
    bool Bad = false;
};

// Console resolution (design: "Console addressing"). headerConsole 0xFFFF
// picks the pinned default if one is set (-1 = none), failing with no_console
// if that console no longer exists (no fallback), else the lowest existing id.
// Any other value must be an existing id. Returns ErrorCode::none on success.
ErrorCode ResolveConsole(uint16_t headerConsole, int pinnedDefault, const std::vector<int>& existingIds, int& out);

// ---------------------------------------------------------------------------
// Writer / Reader

class Writer
{
public:
    static constexpr bool Writing = true;

    std::vector<uint8_t> Buf;

    void U8(uint8_t v) { Buf.push_back(v); }
    void U16(uint16_t v) { U8(uint8_t(v)); U8(uint8_t(v >> 8)); }
    void U32(uint32_t v) { U16(uint16_t(v)); U16(uint16_t(v >> 16)); }
    void U64(uint64_t v) { U32(uint32_t(v)); U32(uint32_t(v >> 32)); }
    void Bytes(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; Buf.insert(Buf.end(), b, b + n); }
    void String(const std::string& s) { U32(uint32_t(s.size())); Bytes(s.data(), s.size()); }

    bool Ok() const { return Good; }
    void Fail() { Good = false; }

    template <class T>
    std::enable_if_t<std::is_integral_v<T>> Io(T& v)
    {
        typedef std::make_unsigned_t<T> U;
        U u = U(v);
        for (size_t i = 0; i < sizeof(U); i++)
            U8(uint8_t(u >> (8 * i)));
    }
    template <class E>
    std::enable_if_t<std::is_enum_v<E>> Io(E& v)
    {
        auto u = std::underlying_type_t<E>(v);
        Io(u);
    }
    void Io(std::string& s) { String(s); }
    void IoBytes(std::vector<uint8_t>& v) { U32(uint32_t(v.size())); Bytes(v.data(), v.size()); } // u32 length + bytes
    void Rest(std::vector<uint8_t>& v) { Bytes(v.data(), v.size()); }                           // raw to end of body
    void Pad(size_t n) { Buf.insert(Buf.end(), n, 0); }
    // Trailing optional field: always written; a Reader at the end of the body reads it as 0.
    void OptU8(uint8_t& v) { U8(v); }

    // Elements of an already-counted list (the count was written separately).
    template <class T, class F>
    void Items(std::vector<T>& v, size_t, size_t, F fn)
    {
        for (T& e : v)
            fn(*this, e);
    }
    // u8/u16/u32 count followed by the elements. minElem is only used when reading.
    template <class C, class T, class F>
    void List(std::vector<T>& v, size_t minElem, F fn)
    {
        if (v.size() > size_t(std::numeric_limits<C>::max()))
            Good = false;
        C n = C(v.size());
        Io(n);
        Items(v, v.size(), minElem, fn);
    }

private:
    bool Good = true;
};

class Reader
{
public:
    static constexpr bool Writing = false;

    Reader(const uint8_t* data, size_t len) : P(data), N(len) {}

    // Every read returns 0/empty and latches failure on underrun.
    uint8_t U8() { return uint8_t(Take(1)); }
    uint16_t U16() { return uint16_t(Take(2)); }
    uint32_t U32() { return uint32_t(Take(4)); }
    uint64_t U64() { return Take(8); }
    std::string String()
    {
        uint32_t n = U32();
        if (!Need(n))
            return std::string();
        std::string s((const char*)P + Pos, n);
        Pos += n;
        return s;
    }

    bool Ok() const { return Good; }
    bool AtEnd() const { return Pos == N; }
    size_t Remaining() const { return N - Pos; }
    void Fail() { Good = false; }

    template <class T>
    std::enable_if_t<std::is_integral_v<T>> Io(T& v)
    {
        typedef std::make_unsigned_t<T> U;
        v = T(U(Take(sizeof(U))));
    }
    template <class E>
    std::enable_if_t<std::is_enum_v<E>> Io(E& v)
    {
        std::underlying_type_t<E> u = 0;
        Io(u);
        v = E(u);
    }
    void Io(std::string& s) { s = String(); }
    void IoBytes(std::vector<uint8_t>& v)
    {
        uint32_t n = U32();
        v.clear();
        if (!Need(n))
            return;
        v.assign(P + Pos, P + Pos + n);
        Pos += n;
    }
    void Rest(std::vector<uint8_t>& v)
    {
        v.assign(P + Pos, P + N);
        Pos = N;
    }
    void Pad(size_t n)
    {
        if (Need(n))
            Pos += n;
    }
    // Trailing optional field: 0 when the body ends before it, so older clients still decode.
    void OptU8(uint8_t& v) { v = AtEnd() ? 0 : U8(); }

    template <class T, class F>
    void Items(std::vector<T>& v, size_t n, size_t minElem, F fn)
    {
        v.clear();
        // Every element takes at least minElem bytes, so a lying count cannot force a huge allocation.
        if (!Good || (minElem && n > Remaining() / minElem))
        {
            Good = false;
            return;
        }
        v.resize(n);
        for (size_t i = 0; i < n && Good; i++)
            fn(*this, v[i]);
    }
    template <class C, class T, class F>
    void List(std::vector<T>& v, size_t minElem, F fn)
    {
        C n = 0;
        Io(n);
        Items(v, size_t(n), minElem, fn);
    }

private:
    bool Need(size_t n)
    {
        if (!Good || n > N - Pos)
        {
            Good = false;
            return false;
        }
        return true;
    }
    uint64_t Take(size_t n)
    {
        if (!Need(n))
            return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < n; i++)
            v |= uint64_t(P[Pos + i]) << (8 * i);
        Pos += n;
        return v;
    }

    const uint8_t* P;
    size_t N;
    size_t Pos = 0;
    bool Good = true;
};

// Per-message extra validation hook, found by overload (default: nothing).
template <class S> bool CheckBody(const S&) { return true; }

// Encode/decode a body. Encode returns an empty vector if a list was too long
// for its count field. Decode fails on underrun, trailing bytes or a failed check.
template <class S>
std::vector<uint8_t> Encode(const S& s)
{
    Writer w;
    const_cast<S&>(s).Visit(w);
    if (!w.Ok())
        return std::vector<uint8_t>();
    return std::move(w.Buf);
}

template <class S>
bool Decode(const uint8_t* data, size_t len, S& out)
{
    Reader r(data, len);
    out.Visit(r);
    return r.Ok() && r.AtEnd() && CheckBody(out);
}

template <class S>
bool Decode(const std::vector<uint8_t>& body, S& out)
{
    return Decode(body.data(), body.size(), out);
}

// A complete message (header + body) for a message struct.
template <class S>
std::vector<uint8_t> Pack(uint16_t console, const S& s)
{
    return BuildMessage(S::Type, console, Encode(s));
}

// Symbol names (with_symbols): a reply to a request with with_symbols set ends with an optional section
// {u32 count, count x string}: one name per entry of the list it annotates, in order, empty when the address has
// no symbol. The section is absent when with_symbols was not set (hasNames = 0), so replies keep their old layout.
template <class A>
inline void IoNames(A& a, uint8_t& hasNames, std::vector<std::string>& names)
{
    auto item = [](auto& x, std::string& n) { x.Io(n); };
    if constexpr (A::Writing)
    {
        if (hasNames)
            a.template List<uint32_t>(names, 4, item);
    }
    else if (a.Remaining())
    {
        hasNames = 1;
        a.template List<uint32_t>(names, 4, item);
    }
}

// ---------------------------------------------------------------------------
// Generic shapes shared by many messages

// {u32 seq}
template <MsgType T>
struct SeqOnly
{
    static constexpr MsgType Type = T;
    uint32_t Seq = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); }
};

// {u32 seq, u8 enabled}
template <MsgType T>
struct EnabledMsg
{
    static constexpr MsgType Type = T;
    uint32_t Seq = 0;
    uint8_t Enabled = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Enabled); }
};

// {u32 seq, u32 value}
template <MsgType T>
struct U32Msg
{
    static constexpr MsgType Type = T;
    uint32_t Seq = 0;
    uint32_t Value = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Value); }
};

// {u32 seq, string text}
template <MsgType T>
struct StringMsg
{
    static constexpr MsgType Type = T;
    uint32_t Seq = 0;
    std::string Text;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Text); }
};

// {u32 seq, u32 max}
template <MsgType T>
struct MaxMsg
{
    static constexpr MsgType Type = T;
    uint32_t Seq = 0;
    uint32_t Max = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Max); }
};

// {u32 seq, u8 cpu, u32 max}
template <MsgType T>
struct CpuMaxMsg
{
    static constexpr MsgType Type = T;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Max = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Max); }
};

// ---------------------------------------------------------------------------
// Replies. Each request type lists its reply below.

// ok: {u32 seq}
typedef SeqOnly<MsgType::ok> OkReply;

// error: {u32 seq, u16 code, string text}
struct ErrorReply
{
    static constexpr MsgType Type = MsgType::error;
    uint32_t Seq = 0;
    ErrorCode Code = ErrorCode::internal;
    std::string Text;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Code); a.Io(Text); }
};

// data whose payload is just the rest of the body: {u32 seq, bytes...}.
// Reply to read.
struct BytesReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    std::vector<uint8_t> Data;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Rest(Data); }
};

// data {u32 seq, i32 value}. Reply to breakpoint_add, watchpoint_add (point id).
struct IdReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    int32_t Id = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Id); }
};

// data {u32 seq, u16 count, count x {u16 id, u8 state (0 idle, 1 running, 2 parked), u64 frames}}.
// Reply to console_list (frames = the console's frame count now).
struct ConsoleInfo
{
    uint16_t Id = 0;
    uint8_t State = 0;
    uint64_t Frames = 0;
};

struct ConsoleListReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    std::vector<ConsoleInfo> Consoles;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq);
        a.template List<uint16_t>(Consoles, 11, [](auto& x, ConsoleInfo& c) { x.Io(c.Id); x.Io(c.State); x.Io(c.Frames); });
    }
};

// ---------------------------------------------------------------------------
// Session

// hello (optional: a client that skips it gets the defaults) request: {u32 seq, u32 protocolVersion, u16 defaultConsole (0xFFFF = lowest),
//                 u8 emptyQueue (0 repeat, 1 neutral, 2 wait), u8 reserved,
//                 u16 stallTimeoutFrames (repeat mode; default 15, 0 = never)}
struct Hello
{
    static constexpr MsgType Type = MsgType::hello;
    uint32_t Seq = 0;
    uint32_t ProtocolVersion = Protocol::ProtocolVersion;
    uint16_t DefaultConsole = Protocol::DefaultConsole;
    uint8_t EmptyQueue = 0;
    uint8_t Reserved = 0;
    uint16_t StallTimeoutFrames = 15;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(ProtocolVersion); a.Io(DefaultConsole);
        a.Io(EmptyQueue); a.Io(Reserved); a.Io(StallTimeoutFrames);
    }
};

// hello reply (data): {u32 seq, u32 protocolVersion, string name, u32 pid, u16 port,
//                      u16 defaultConsole (the resolved default now, 0xFFFF if none),
//                      u16 count, u16 ids[count]}
struct HelloReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint32_t ProtocolVersion = Protocol::ProtocolVersion;
    std::string Name;
    uint32_t Pid = 0;
    uint16_t Port = 0;
    uint16_t DefaultConsole = Protocol::DefaultConsole;
    std::vector<uint16_t> Ids;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(ProtocolVersion); a.Io(Name); a.Io(Pid); a.Io(Port); a.Io(DefaultConsole);
        a.template List<uint16_t>(Ids, 2, [](auto& x, uint16_t& v) { x.Io(v); });
    }
};

// set_option: {u32 seq, u16 key, u16 reserved, u32 value}; reply ok.
// Keys: OptEmptyQueue, OptStallTimeoutFrames (header console = target, 0xFFFF = all),
// OptDefaultConsole (applies to the connection). Unknown key or value: bad_option.
struct SetOption
{
    static constexpr MsgType Type = MsgType::set_option;
    uint32_t Seq = 0;
    uint16_t Key = 0;
    uint16_t Reserved = 0;
    uint32_t Value = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Key); a.Io(Reserved); a.Io(Value); }
};

// ---------------------------------------------------------------------------
// Lifecycle

// console_create: {u32 seq, string bios9, string bios7, string firmware, string nickname, u8 debug}
// reply data: {u32 seq, u16 id}
struct ConsoleCreate
{
    static constexpr MsgType Type = MsgType::console_create;
    uint32_t Seq = 0;
    std::string Bios9, Bios7, Firmware, Nickname;
    uint8_t Debug = 0;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Bios9); a.Io(Bios7); a.Io(Firmware); a.Io(Nickname); a.Io(Debug);
    }
};

struct ConsoleCreateReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint16_t Id = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Id); }
};

// console_destroy, console_list (reply ConsoleListReply), reset, shutdown: {u32 seq}; reply ok. console_destroy
// sends a console_gone event first. reset and rom_load clear the input queue and send input_queue(0) first.
// shutdown: ok, then the server closes the connection and mds_serve_wait() returns 1 for the host process.
typedef SeqOnly<MsgType::console_destroy> ConsoleDestroy;
typedef SeqOnly<MsgType::console_list> ConsoleList;
typedef SeqOnly<MsgType::reset> Reset;
typedef SeqOnly<MsgType::shutdown> Shutdown;

// rom_load: {u32 seq, string rom, string save}; reply ok
struct RomLoad
{
    static constexpr MsgType Type = MsgType::rom_load;
    uint32_t Seq = 0;
    std::string Rom, Save;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Rom); a.Io(Save); }
};

// ---------------------------------------------------------------------------
// Execution. Console lists: u16 count + u16 ids, empty = every console.

template <class A>
inline void IoIds(A& a, std::vector<uint16_t>& ids)
{
    a.template List<uint16_t>(ids, 2, [](auto& x, uint16_t& v) { x.Io(v); });
}

// run: {u32 seq, u32 frames, u16 count, u16 ids[count]}. The ok reply is DEFERRED until every named
// console (empty list = all) has stopped: finished its frames or parked at a break (then the others are
// interrupted and a break event precedes the reply). Other messages are served meanwhile.
struct Run
{
    static constexpr MsgType Type = MsgType::run;
    uint32_t Seq = 0;
    uint32_t Frames = 0;
    std::vector<uint16_t> Consoles;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Frames); IoIds(a, Consoles); }
};

// live_start: {u32 seq, u32 fps, u16 count, u16 ids[count]}; starts the consoles (empty = all) with no end and
// sets the process pacing to fps (0 = unlimited); reply ok at once
struct LiveStart
{
    static constexpr MsgType Type = MsgType::live_start;
    uint32_t Seq = 0;
    uint32_t Fps = 0;
    std::vector<uint16_t> Consoles;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Fps); IoIds(a, Consoles); }
};

// live_stop: {u32 seq, u16 count, u16 ids[count]}; stops at the next frame boundary; reply ok once every named
// console (empty = all) has stopped
struct LiveStop
{
    static constexpr MsgType Type = MsgType::live_stop;
    uint32_t Seq = 0;
    std::vector<uint16_t> Consoles;
    template <class A> void Visit(A& a) { a.Io(Seq); IoIds(a, Consoles); }
};

// pause: {u32 seq}; stops the header console at the next frame boundary; reply ok once it has stopped
typedef SeqOnly<MsgType::pause> Pause;

// step: {u32 seq, u8 cpu, u32 count}; the console must be stopped. Arms the step and runs the console until it
// parks on it (at most 120 frames); reply ok (deferred, as run) once it has stopped, after the break event.
struct Step
{
    static constexpr MsgType Type = MsgType::step;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Count = 1;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Count); }
};

// ---------------------------------------------------------------------------
// Input

// One TouchPoint is a 12-byte record: {u32 offset (system cycles from frame
// start, 0..FrameCycles-1), u16 x (0..255), u16 y (0..191), u8 down (0 =
// released from this point on), u8 reserved[3] (zero)}.
struct TouchPoint
{
    uint32_t Offset = 0;
    uint16_t X = 0;
    uint16_t Y = 0;
    uint8_t Down = 0;
    template <class A> void Visit(A& a) { a.Io(Offset); a.Io(X); a.Io(Y); a.Io(Down); a.Pad(3); }
};

// InputPayload, the body of `input` (reply data {u32 seq, u16 queueDepth}, see InputReply).
// 14 fixed bytes, then the lists:
//   u32 seq           request seq, echoed in the frame event that applies it
//   u16 buttons       bit per key, mds_set_keys order (12 bits)
//   u8  lid           0 open, 1 closed
//   u8  touchCount    0 = no change from last frame, else points follow (max 16)
//   u8  micKind       0 none, 1 samples, 2 level
//   u8  reserved
//   u16 micCount      samples that follow (micKind 1), else 0
//   u16 micLevel      0..65535 (micKind 2)
//   TouchPoint points[touchCount]   (12 bytes each)
//   s16 mic[micCount]
struct InputPayload
{
    static constexpr MsgType Type = MsgType::input;
    uint32_t Seq = 0;
    uint16_t Buttons = 0;
    uint8_t Lid = 0;
    uint8_t MicKind = 0;
    uint8_t Reserved = 0;
    uint16_t MicLevel = 0;
    std::vector<TouchPoint> Touch;
    std::vector<int16_t> Mic;

    template <class A> void Visit(A& a)
    {
        uint8_t touchCount = 0;
        uint16_t micCount = 0;
        if (A::Writing)
        {
            touchCount = uint8_t(Touch.size());
            micCount = uint16_t(Mic.size());
            if (Touch.size() > 255 || Mic.size() > 65535)
                a.Fail();
        }
        a.Io(Seq); a.Io(Buttons); a.Io(Lid); a.Io(touchCount); a.Io(MicKind); a.Io(Reserved);
        a.Io(micCount); a.Io(MicLevel);
        if (!A::Writing && touchCount > MaxTouchPoints)
            a.Fail();
        a.Items(Touch, touchCount, 12, [](auto& x, TouchPoint& t) { t.Visit(x); });
        a.Items(Mic, micCount, 2, [](auto& x, int16_t& s) { x.Io(s); });
    }
};

// Semantic checks for a decoded payload (decode only checks structure and
// the 16-point cap): buttons < 0x1000, lid <= 1, micKind <= 2, mic samples
// only with micKind 1 and at most MaxMicSamples, touch offsets strictly
// increasing and < FrameCycles, x <= 255, y <= 191, down <= 1.
bool ValidatePayload(const InputPayload& p);

struct InputReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint16_t Depth = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Depth); }
};

// input_multi: {u32 seq, u16 count, count x {u16 console, InputPayload}}; reply
// data {u32 seq, u16 count, u16 depth[count]} (queue depth after queueing, in entry order). Marks the payloads a
// barrier group. The header console is ignored; each entry names its console by literal id. All or nothing:
// no entries / duplicate console / bad payload -> bad_payload, unknown console -> no_console, any full queue ->
// input_queue_full, and nothing is queued. A console whose next payload belongs to a group waits at its frame
// boundary until every member has reached its payload, then all apply and start together. A member that cannot
// arrive (console destroyed, stopped after its frames ran out, queue cleared by reset / savestate load /
// disconnect) stops counting: the others proceed, and that member does not wait if it later reaches its payload.
// A wait is also cut short while another member is blocked in a wireless receive: the waiter then applies its own
// payload at once (early, not skipped), so every member still applies its payload on the same frame of its own
// numbering. A paused/parked member holds the barrier; stop or interrupt releases the waiter as in wait mode.
struct InputMultiEntry
{
    uint16_t Console = 0;
    InputPayload Payload;
};

struct InputMulti
{
    static constexpr MsgType Type = MsgType::input_multi;
    uint32_t Seq = 0;
    std::vector<InputMultiEntry> Entries;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq);
        a.template List<uint16_t>(Entries, 16, [](auto& x, InputMultiEntry& e) { x.Io(e.Console); e.Payload.Visit(x); });
    }
};

struct InputMultiReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    std::vector<uint16_t> Depths;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq);
        a.template List<uint16_t>(Depths, 2, [](auto& x, uint16_t& v) { x.Io(v); });
    }
};

// input_log_read: {u32 seq, u64 fromFrame, u32 max}
// reply data: {u32 seq, u8 truncated, u32 count, count x {u64 frame, InputPayload}}
// (payload seq = the sender's seq, 0 for Lua and in-process sources)
struct InputLogRead
{
    static constexpr MsgType Type = MsgType::input_log_read;
    uint32_t Seq = 0;
    uint64_t FromFrame = 0;
    uint32_t Max = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(FromFrame); a.Io(Max); }
};

struct InputLogEntry
{
    uint64_t Frame = 0;
    InputPayload Payload;
};

struct InputLogReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<InputLogEntry> Entries;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Entries, 22, [](auto& x, InputLogEntry& e) { x.Io(e.Frame); e.Payload.Visit(x); });
    }
};

// ---------------------------------------------------------------------------
// Memory

// read: {u32 seq, u8 cpu, u32 addr, u32 len}; reply BytesReply (the bytes)
struct ReadReq
{
    static constexpr MsgType Type = MsgType::read;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Addr = 0;
    uint32_t Len = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Addr); a.Io(Len); }
};

// write: {u32 seq, u8 cpu, u32 addr, bytes data (u32 length + bytes)}; reply ok
struct WriteReq
{
    static constexpr MsgType Type = MsgType::write;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Addr = 0;
    std::vector<uint8_t> Data;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Addr); a.IoBytes(Data); }
};

// registers: {u32 seq, u8 cpu}
// reply data: {u32 seq, u16 count, u32 regs[count]}; the order is mds_get_regs:
// r0-r14, pc, cpsr, spsr, raw r15 (19 values)
struct RegistersReq
{
    static constexpr MsgType Type = MsgType::registers;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); }
};

struct RegistersReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    std::vector<uint32_t> Regs;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq);
        a.template List<uint16_t>(Regs, 4, [](auto& x, uint32_t& v) { x.Io(v); });
    }
};

// set_register: {u32 seq, u8 cpu, u8 reg, u32 value}; reply ok
struct SetRegister
{
    static constexpr MsgType Type = MsgType::set_register;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint8_t Reg = 0;
    uint32_t Value = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Reg); a.Io(Value); }
};

// screenshot: {u32 seq}; reply data {u32 seq, u64 frame, ScreenshotBytes of BGRA}
typedef SeqOnly<MsgType::screenshot> Screenshot;

struct ScreenshotReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint64_t Frame = 0;
    std::vector<uint8_t> Pixels;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Frame); a.Rest(Pixels); }
};
template <> inline bool CheckBody(const ScreenshotReply& r) { return r.Pixels.size() == ScreenshotBytes; }

// ---------------------------------------------------------------------------
// Search

// Range rule: length == 0 selects the default range (start 0x02000000, 4 MB)
// and `start` is ignored; otherwise start/length are used as given.
inline uint32_t ResolveStart(uint32_t start, uint32_t length) { return length == 0 ? DefaultSearchStart : start; }
inline uint32_t ResolveLength(uint32_t, uint32_t length) { return length == 0 ? DefaultSearchLength : length; }
// limit 0 selects the default; above MaxSearchLimit is clamped.
inline uint32_t ResolveSearchLimit(uint32_t limit, uint32_t def)
{
    return limit == 0 ? def : (limit > MaxSearchLimit ? MaxSearchLimit : limit);
}
// encodings 0 selects all three
inline uint8_t ResolveEncodings(uint8_t mask) { return mask == 0 ? EncAll : uint8_t(mask & EncAll); }

// memory_search: {u32 seq, u8 cpu, u32 start, u32 length, u8 size, u32 limit, bytes needle}
//   size 1/2/4: needle is exactly `size` bytes, a little-endian value matched at
//   size-aligned offsets. size 0: needle is a non-empty byte string matched at
//   any offset. Reply data: {u32 seq, u8 truncated, u32 count, u32 addrs[count]}
struct MemorySearch
{
    static constexpr MsgType Type = MsgType::memory_search;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Start = 0;
    uint32_t Length = 0;
    uint8_t Size = 0;
    uint32_t Limit = 0;
    std::vector<uint8_t> Needle;
    uint8_t WithSymbols = 0;    // trailing optional u8: 1 = add symbol names to the reply (see IoNames)
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Cpu); a.Io(Start); a.Io(Length); a.Io(Size); a.Io(Limit); a.IoBytes(Needle);
        a.OptU8(WithSymbols);
    }
};
bool ValidateSearch(const MemorySearch& s);

struct MemorySearchReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<uint32_t> Addrs;
    uint8_t HasNames = 0;
    std::vector<std::string> Names;     // one per address when HasNames
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Addrs, 4, [](auto& x, uint32_t& v) { x.Io(v); });
        IoNames(a, HasNames, Names);
    }
};

// text_find: {u32 seq, u8 cpu, u32 start, u32 length, string text (UTF-8), u8 encodings, u32 limit}
//   Reply data: {u32 seq, u8 truncated, u32 count, count x {u32 address, u8 encoding}}
//   where encoding is one of EncAscii, EncUtf16le, EncUtf8. Hits ascend by address then encoding. Pure ASCII text
//   has the same bytes in ASCII and UTF-8: it is reported once, as EncAscii when that is selected.
//   The request may end with an optional u8 withSymbols; the reply then ends with the names section (IoNames).
struct TextFind
{
    static constexpr MsgType Type = MsgType::text_find;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Start = 0;
    uint32_t Length = 0;
    std::string Text;
    uint8_t Encodings = 0;
    uint32_t Limit = 0;
    uint8_t WithSymbols = 0;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Cpu); a.Io(Start); a.Io(Length); a.Io(Text); a.Io(Encodings); a.Io(Limit);
        a.OptU8(WithSymbols);
    }
};
template <> inline bool CheckBody(const TextFind& t) { return !t.Text.empty(); }

struct TextHit
{
    uint32_t Addr = 0;
    uint8_t Encoding = 0;
};

struct TextFindReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<TextHit> Hits;
    uint8_t HasNames = 0;
    std::vector<std::string> Names;     // one per hit when HasNames
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Hits, 5, [](auto& x, TextHit& h) { x.Io(h.Addr); x.Io(h.Encoding); });
        IoNames(a, HasNames, Names);
    }
};

// ---------------------------------------------------------------------------
// Subscriptions

struct SubRange
{
    uint8_t Cpu = 0;
    uint32_t Addr = 0;
    uint32_t Len = 0;
};

// subscribe: {u32 seq, u32 id, u32 everyNFrames (0 or 1 = every frame), u16 count,
//             count x {u8 cpu, u32 addr, u32 len}}; reply ok. Max MaxSubscribeRanges ranges.
struct Subscribe
{
    static constexpr MsgType Type = MsgType::subscribe;
    uint32_t Seq = 0;
    uint32_t Id = 0;
    uint32_t EveryNFrames = 1;
    std::vector<SubRange> Ranges;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Id); a.Io(EveryNFrames);
        a.template List<uint16_t>(Ranges, 9, [](auto& x, SubRange& r) { x.Io(r.Cpu); x.Io(r.Addr); x.Io(r.Len); });
    }
};
template <> inline bool CheckBody(const Subscribe& s) { return s.Ranges.size() <= MaxSubscribeRanges; }

// unsubscribe: {u32 seq, u32 id}; reply ok
typedef U32Msg<MsgType::unsubscribe> Unsubscribe;

// ---------------------------------------------------------------------------
// Events (seq = 0, header console = source console)

struct SubBlock
{
    uint32_t SubId = 0;
    std::vector<std::vector<uint8_t>> Ranges; // one entry per subscribed range, in subscription order
};

// A console only sends frame events while the client has a subscription on it (zero ranges = plain per-frame
// notifications). waitingForInput: Wait mode with an empty queue, i.e. the console stops at the next frame
// boundary until input arrives. dropped: frame events discarded for back-pressure (Outbox) before this one.
// frame: {u32 seq=0, u64 frame, u32 appliedSeq, u8 inputApplied, u8 waitingForInput,
//         u16 queueDepth, u32 dropped (frame events dropped for back-pressure since the last one),
//         u16 subCount, subCount x {u32 subId, u16 rangeCount, rangeCount x {u32 len, bytes}}}
struct FrameEvent
{
    static constexpr MsgType Type = MsgType::frame;
    uint32_t Seq = 0;
    uint64_t Frame = 0;
    uint32_t AppliedSeq = 0;
    uint8_t InputApplied = 0;
    uint8_t WaitingForInput = 0;
    uint16_t QueueDepth = 0;
    uint32_t Dropped = 0;
    std::vector<SubBlock> Blocks;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Frame); a.Io(AppliedSeq); a.Io(InputApplied); a.Io(WaitingForInput);
        a.Io(QueueDepth); a.Io(Dropped);
        a.template List<uint16_t>(Blocks, 6, [](auto& x, SubBlock& b) {
            x.Io(b.SubId);
            x.template List<uint16_t>(b.Ranges, 4, [](auto& y, std::vector<uint8_t>& r) { y.IoBytes(r); });
        });
    }
};

// break_: {u32 seq=0, i32 kind, i32 cpu, i32 size, i32 id, u32 pc, u32 addr, u32 value, i32 dma}
//   kind as mds_break (melonds_api.h); kind 6 (API level 6) is a GDB reset of the running console, not a park.
// (mds_break field for field)
struct BreakEvent
{
    static constexpr MsgType Type = MsgType::break_;
    uint32_t Seq = 0;
    int32_t Kind = 0, Cpu = 0, Size = 0, Id = 0;
    uint32_t Pc = 0, Addr = 0, Value = 0;
    int32_t Dma = -1;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Kind); a.Io(Cpu); a.Io(Size); a.Io(Id); a.Io(Pc); a.Io(Addr); a.Io(Value); a.Io(Dma);
    }
};

// lua_output: {u32 seq=0, string text}
typedef StringMsg<MsgType::lua_output> LuaOutputEvent;

// input_queue: {u32 seq=0, u16 depth}
struct InputQueueEvent
{
    static constexpr MsgType Type = MsgType::input_queue;
    uint32_t Seq = 0;
    uint16_t Depth = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Depth); }
};

// console_gone: {u32 seq=0}; header console = the destroyed id
typedef SeqOnly<MsgType::console_gone> ConsoleGoneEvent;

// ---------------------------------------------------------------------------
// Debug and analysis. All except packets_* need a stopped console
// (else error console_running). "Clearing" reads return what was collected
// since the last read and reset it; their replies carry a u8 truncated flag
// meaning more is still waiting.

// breakpoint_add: {u32 seq, u32 cpuMask, u32 addr}; reply IdReply
struct BreakpointAdd
{
    static constexpr MsgType Type = MsgType::breakpoint_add;
    uint32_t Seq = 0;
    uint32_t CpuMask = 0;
    uint32_t Addr = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(CpuMask); a.Io(Addr); }
};

// watchpoint_add: {u32 seq, u32 cpuMask, u32 addr, u32 size, u32 flags}; reply IdReply
struct WatchpointAdd
{
    static constexpr MsgType Type = MsgType::watchpoint_add;
    uint32_t Seq = 0;
    uint32_t CpuMask = 0;
    uint32_t Addr = 0;
    uint32_t Size = 0;
    uint32_t Flags = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(CpuMask); a.Io(Addr); a.Io(Size); a.Io(Flags); }
};

// point_remove: {u32 seq, i32 id}; reply ok
struct PointRemove
{
    static constexpr MsgType Type = MsgType::point_remove;
    uint32_t Seq = 0;
    int32_t Id = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Id); }
};

// point_enable: {u32 seq, i32 id, u8 enabled}; reply ok
struct PointEnable
{
    static constexpr MsgType Type = MsgType::point_enable;
    uint32_t Seq = 0;
    int32_t Id = 0;
    uint8_t Enabled = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Id); a.Io(Enabled); }
};

// point_list: {u32 seq}
typedef SeqOnly<MsgType::point_list> PointList;

// reply data: {u32 seq, u32 count, count x PointInfo} with PointInfo =
// {i32 id, u8 watch (0 exec, 1 watch), u8 enabled, u32 cpuMask, u32 flags
//  (read/write bits, mds_point.flags; 0x100 = a Lua hook from API level 6), u32 start, u32 end, u64 hits} = 30 bytes
struct PointInfo
{
    int32_t Id = 0;
    uint8_t Watch = 0;
    uint8_t Enabled = 0;
    uint32_t CpuMask = 0;
    uint32_t Flags = 0;
    uint32_t Start = 0;
    uint32_t End = 0;
    uint64_t Hits = 0;
};

struct PointListReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    std::vector<PointInfo> Points;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq);
        a.template List<uint32_t>(Points, 30, [](auto& x, PointInfo& p) {
            x.Io(p.Id); x.Io(p.Watch); x.Io(p.Enabled); x.Io(p.CpuMask); x.Io(p.Flags);
            x.Io(p.Start); x.Io(p.End); x.Io(p.Hits);
        });
    }
};

// savestate_save / savestate_load: {u32 seq, string path}; reply ok
typedef StringMsg<MsgType::savestate_save> SavestateSave;
typedef StringMsg<MsgType::savestate_load> SavestateLoad;

// lua_run: {u32 seq, u8 isFile, string source}; lua_exec: {u32 seq, string code};
// lua_stop: {u32 seq}; reply ok
struct LuaRun
{
    static constexpr MsgType Type = MsgType::lua_run;
    uint32_t Seq = 0;
    uint8_t IsFile = 0;
    std::string Source;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(IsFile); a.Io(Source); }
};
typedef StringMsg<MsgType::lua_exec> LuaExec;
typedef SeqOnly<MsgType::lua_stop> LuaStop;

// trace_config: {u32 seq, u32 cpuMask, u32 start, u32 end, u32 capacity (0 = off)}; reply ok
struct TraceConfig
{
    static constexpr MsgType Type = MsgType::trace_config;
    uint32_t Seq = 0;
    uint32_t CpuMask = 0;
    uint32_t Start = 0;
    uint32_t End = 0;
    uint32_t Capacity = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(CpuMask); a.Io(Start); a.Io(End); a.Io(Capacity); }
};

// trace_read: {u32 seq, u32 max, [u8 withSymbols]}; clearing (max 0 = DefaultReadMax); when it stops at max the
// rest stays for the next read and truncated is 1. Reply data: {u32 seq, u8 truncated, u32 count,
// count x {u64 cycle, u8 cpu, u32 pc, u32 opcode, u32 cpsr}} oldest first (21 bytes each), then with_symbols
// names (one per pc).
struct TraceRead
{
    static constexpr MsgType Type = MsgType::trace_read;
    uint32_t Seq = 0;
    uint32_t Max = 0;
    uint8_t WithSymbols = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Max); a.OptU8(WithSymbols); }
};

struct TraceItem
{
    uint64_t Cycle = 0;
    uint8_t Cpu = 0;
    uint32_t Pc = 0;
    uint32_t Opcode = 0;
    uint32_t Cpsr = 0;
};

struct TraceReadReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<TraceItem> Entries;
    uint8_t HasNames = 0;
    std::vector<std::string> Names;     // one per entry (its pc) when HasNames
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Entries, 21, [](auto& x, TraceItem& e) {
            x.Io(e.Cycle); x.Io(e.Cpu); x.Io(e.Pc); x.Io(e.Opcode); x.Io(e.Cpsr);
        });
        IoNames(a, HasNames, Names);
    }
};

// calls_track / coverage_track / writers_track: {u32 seq, u8 enabled}; reply ok
typedef EnabledMsg<MsgType::calls_track> CallsTrack;
typedef EnabledMsg<MsgType::coverage_track> CoverageTrack;
typedef EnabledMsg<MsgType::writers_track> WritersTrack;

// callstack: {u32 seq, u8 cpu, [u8 withSymbols]}. Reply data: {u32 seq, u32 count, count x CallFrame}, innermost
// first, CallFrame = {u32 site, u32 entry, u32 ret, u32 sp, u32 mode, u8 exception} (mds_frame), then with_symbols
// names: the entry names (one per frame), then the call-site names (one per frame) in a second names section.
struct CallStackReq
{
    static constexpr MsgType Type = MsgType::callstack;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint8_t WithSymbols = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.OptU8(WithSymbols); }
};

struct CallFrame
{
    uint32_t Site = 0;
    uint32_t Entry = 0;
    uint32_t Ret = 0;
    uint32_t Sp = 0;
    uint32_t Mode = 0;
    uint8_t Exception = 0;
};

struct CallStackReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    std::vector<CallFrame> Frames;
    uint8_t HasNames = 0;
    std::vector<std::string> Names;         // entry name per frame when HasNames
    std::vector<std::string> SiteNames;     // call-site name per frame when HasNames
    template <class A> void Visit(A& a)
    {
        a.Io(Seq);
        a.template List<uint32_t>(Frames, 21, [](auto& x, CallFrame& f) {
            x.Io(f.Site); x.Io(f.Entry); x.Io(f.Ret); x.Io(f.Sp); x.Io(f.Mode); x.Io(f.Exception);
        });
        IoNames(a, HasNames, Names);
        if (HasNames)
        {
            uint8_t again = 1;
            IoNames(a, again, SiteNames);
        }
    }
};

// call_counts_read: {u32 seq, u8 cpu, u32 max, [u8 withSymbols]}; clearing (as trace_read). Reply data: {u32 seq,
// u8 truncated, u32 count, count x {u32 entry, u64 count}}, then names (one per entry).
struct CallCountsRead
{
    static constexpr MsgType Type = MsgType::call_counts_read;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Max = 0;
    uint8_t WithSymbols = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Max); a.OptU8(WithSymbols); }
};

struct CallCount
{
    uint32_t Entry = 0;
    uint64_t Count = 0;
};

struct CallCountsReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<CallCount> Counts;
    uint8_t HasNames = 0;
    std::vector<std::string> Names;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Counts, 12, [](auto& x, CallCount& c) { x.Io(c.Entry); x.Io(c.Count); });
        IoNames(a, HasNames, Names);
    }
};

// coverage_read: {u32 seq, u8 cpu, u32 max, [u8 withSymbols]}; clearing (as trace_read). Reply data: {u32 seq,
// u8 truncated, u32 count, u32 addrs[count]}, then names (one per address).
struct CoverageRead
{
    static constexpr MsgType Type = MsgType::coverage_read;
    uint32_t Seq = 0;
    uint8_t Cpu = 0;
    uint32_t Max = 0;
    uint8_t WithSymbols = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Cpu); a.Io(Max); a.OptU8(WithSymbols); }
};

struct CoverageReadReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<uint32_t> Addrs;
    uint8_t HasNames = 0;
    std::vector<std::string> Names;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Addrs, 4, [](auto& x, uint32_t& v) { x.Io(v); });
        IoNames(a, HasNames, Names);
    }
};

// last_writer: {u32 seq, u32 addr} (main RAM; writers_track must have been on when the write happened).
// Reply data: {u32 seq, u8 kind (0 none, 1 CPU instruction, 2 DMA), u8 cpu (9 or 7), u8 channel (DMA only, 0-3),
// u8 reserved, u32 pc (CPU only, the storing instruction), u32 raw (the packed value mds_lastwriter_get returned)}.
// The packed value is: 0 = none; 0xFFFFFFF0 | channel << 1 | cpu7 = DMA; else pc | cpu7 (pc is halfword aligned).
typedef U32Msg<MsgType::last_writer> LastWriter;

struct LastWriterReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Kind = 0;
    uint8_t Cpu = 0;
    uint8_t Channel = 0;
    uint8_t Reserved = 0;
    uint32_t Pc = 0;
    uint32_t Raw = 0;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Kind); a.Io(Cpu); a.Io(Channel); a.Io(Reserved); a.Io(Pc); a.Io(Raw);
    }
};

// packets_track: {u32 seq, u8 enabled, u32 capacity}; reply ok. Works while running.
struct PacketsTrack
{
    static constexpr MsgType Type = MsgType::packets_track;
    uint32_t Seq = 0;
    uint8_t Enabled = 0;
    uint32_t Capacity = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Enabled); a.Io(Capacity); }
};

// packets_read: {u32 seq, u32 max}; clearing (as trace_read), works while running. Reply data: {u32 seq,
// u8 truncated, u32 count, count x {u64 timestamp, u8 sent, u8 type (0 data, 1 cmd, 2 reply,
// 3 ack), bytes data (u32 length + bytes)}}
typedef MaxMsg<MsgType::packets_read> PacketsRead;

struct PacketItem
{
    uint64_t Timestamp = 0;
    uint8_t Sent = 0;
    uint8_t Type = 0;
    std::vector<uint8_t> Data;
};

struct PacketsReadReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint8_t Truncated = 0;
    std::vector<PacketItem> Packets;
    template <class A> void Visit(A& a)
    {
        a.Io(Seq); a.Io(Truncated);
        a.template List<uint32_t>(Packets, 14, [](auto& x, PacketItem& p) {
            x.Io(p.Timestamp); x.Io(p.Sent); x.Io(p.Type); x.IoBytes(p.Data);
        });
    }
};

// symbols_load: {u32 seq, string path}: loads a symbol file (a plain "ADDR name [size]" list, a dsd .yaml config, or a
// directory holding config.yaml) into the header console's own symbol table, replacing the old one; an empty path
// clears it. Reply data: {u32 seq, u32 symbolCount, u32 moduleCount}; failure is error(failed, text).
// Names requested with with_symbols are resolved against this table and the overlays resident at that moment.
typedef StringMsg<MsgType::symbols_load> SymbolsLoad;

struct SymbolsLoadReply
{
    static constexpr MsgType Type = MsgType::data;
    uint32_t Seq = 0;
    uint32_t Symbols = 0;
    uint32_t Modules = 0;
    template <class A> void Visit(A& a) { a.Io(Seq); a.Io(Symbols); a.Io(Modules); }
};

// ---------------------------------------------------------------------------
// Process-wide

// pacing: {u32 seq, u32 fps}; mp_timeout: {u32 seq, u32 ms}; reply ok
typedef U32Msg<MsgType::pacing> Pacing;
typedef U32Msg<MsgType::mp_timeout> MpTimeout;

}

#endif
