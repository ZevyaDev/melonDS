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

#include "Server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

#include "ApiInternal.h"
#include "Instance.h"
#include "LuaHost.h"
#include "Outbox.h"
#include "Protocol.h"
#include "Search.h"
#include "Socket.h"
#include "melonds_api.h"

namespace P = Headless::Protocol;

namespace Headless
{

namespace
{

constexpr int PollMs = 4;
constexpr int DefaultStall = 15;
constexpr uint32_t StepFrameCap = 120;      // a step that never reaches its instruction count gives up after this

enum class PendingKind
{
    Run,        // reply when every console has stopped; a break interrupts the others
    Stop,       // reply when every console has stopped
};

struct PendingOp
{
    PendingKind Kind;
    uint32_t Seq;
    uint16_t Console;       // header console, echoed in the reply
    std::vector<int> Ids;
    bool Interrupted = false;
};

uint32_t Le32(const std::vector<uint8_t>& b)
{
    return b.size() < 4 ? 0 : uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

bool CpuOk(unsigned cpu)
{
    return cpu == 7 || cpu == 9 || cpu == 0 || cpu == 1;
}

bool Contains(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

// The C API reports failures as text; give each a protocol error code.
P::ErrorCode MapError(const std::string& e)
{
    if (Contains(e, "is running") || Contains(e, "needs an idle"))
        return P::ErrorCode::console_running;
    if (Contains(e, "without the debugger"))
        return P::ErrorCode::not_supported;
    if (e.rfind("no console", 0) == 0 || Contains(e, "shutting down"))
        return P::ErrorCode::no_console;
    if (e.rfind("input_queue_full", 0) == 0)
        return P::ErrorCode::input_queue_full;
    if (e.rfind("bad_payload", 0) == 0)
        return P::ErrorCode::bad_payload;
    return P::ErrorCode::failed;
}

// Symbol names for the addresses of one reply. The console's overlays are refreshed once, on its emulator thread,
// before the first name is resolved.
class Namer
{
public:
    Namer(Instance& inst, bool on) : Syms(inst.Symbols), On(on)
    {
        if (!On)
            return;
        std::shared_ptr<const DebugSymbols::Set> set = Syms.set();
        if (set && !set->Modules.empty())
            inst.Call([&](Instance& i) { i.Symbols.setLoaded(DebugSymbols::detect(*set, *i.NDS)); });
    }

    bool On;

    const std::string& Name(uint32_t addr)
    {
        auto it = Cache.find(addr);
        if (it == Cache.end())
            it = Cache.emplace(addr, Syms.describe(addr)).first;
        return it->second;
    }

    template <class V>
    void Fill(const V& addrs, uint8_t& has, std::vector<std::string>& names)
    {
        has = On;
        if (!On)
            return;
        names.reserve(addrs.size());
        for (uint32_t a : addrs)
            names.push_back(Name(a));
    }

private:
    DebugSymbols& Syms;
    std::unordered_map<uint32_t, std::string> Cache;
};

class Server
{
public:
    ~Server() { Stop(); }

    bool Start(uint32_t address, int port, int range, int& bound, std::string& error);
    void Stop();
    int Wait(int timeoutMs);
    void SetName(const std::string& name)
    {
        std::lock_guard lock(NameLock);
        Name = name;
    }
    bool Running() const { return Thread.joinable(); }

private:
    // ---- connection state, server thread only unless noted
    Net::Socket Listener = Net::InvalidSocket;
    Net::Socket Client = Net::InvalidSocket;
    uint16_t Port = 0;
    std::thread Thread;
    std::atomic<bool> Quit {false};
    std::atomic<bool> Connected {false};
    Outbox Out;
    std::vector<uint8_t> Cur;
    size_t CurOff = 0;
    P::FrameParser Parser;
    std::vector<PendingOp> Pending;
    std::set<int> Started;
    bool CloseAfterFlush = false;
    std::chrono::steady_clock::time_point CloseDeadline;

    // ---- options of the current connection (also read by the created-console hook)
    std::mutex OptLock;
    EmptyMode Mode = EmptyMode::Repeat;
    int Stall = DefaultStall;
    int Pinned = -1;

    std::mutex NameLock;
    std::string Name = "melonds";

    std::mutex ShutdownLock;
    std::condition_variable ShutdownCond;
    bool ShutdownRequested = false;

    void Main();
    void AcceptOne();
    void RejectBusy(Net::Socket s);
    void BeginClient(Net::Socket s);
    void Disconnect();
    bool ReadClient();
    bool Flush();
    void ResetOptions();
    void ApplyOptions(Instance& inst);

    void ServiceBreaks();
    void SendBreaks(int id);
    void ServiceLua();
    void SendLuaOutput(int id);
    void ServicePending();

    // ---- replies
    template <class S>
    void Reply(uint16_t console, const S& s)
    {
        Out.PushReliable(P::Pack(console, s));
    }
    void Ok(uint16_t console, uint32_t seq) { Reply(console, P::OkReply{seq}); }
    void Error(uint16_t console, uint32_t seq, P::ErrorCode code, const std::string& text)
    {
        P::ErrorReply e;
        e.Seq = seq;
        e.Code = code;
        e.Text = text;
        Reply(console, e);
    }
    void FailFromApi(uint16_t console, uint32_t seq)
    {
        std::string text = mds_error();
        Error(console, seq, MapError(text), text);
    }

    template <class S>
    bool Parse(const P::Message& m, S& out)
    {
        if (P::Decode(m.Body, out))
            return true;
        Error(m.Head.Console, Le32(m.Body), P::ErrorCode::bad_payload, "malformed message body");
        return false;
    }

    // Resolve the header console once; on failure the error reply has been sent.
    bool Resolve(const P::Message& m, int& id, Instance*& inst);
    bool ResolveList(const P::Message& m, uint32_t seq, const std::vector<uint16_t>& wanted, std::vector<int>& out);

    void SendInputQueue(int id, size_t depth);
    bool CopyRange(Instance& inst, int cpu, uint32_t start, uint32_t len, std::vector<uint8_t>& out);
    bool SearchRange(const P::Message& m, uint32_t seq, uint32_t start0, uint32_t len0, uint32_t& start, uint32_t& len);
    bool StartAll(const P::Message& m, uint32_t seq, const std::vector<int>& ids, uint32_t frames);

    void Handle(const P::Message& m);
    void DoHello(const P::Message& m);
    void DoSetOption(const P::Message& m);
    void DoConsoleCreate(const P::Message& m);
    void DoConsoleDestroy(const P::Message& m);
    void DoConsoleList(const P::Message& m);
    void DoRomLoad(const P::Message& m);
    void DoReset(const P::Message& m);
    void DoShutdown(const P::Message& m);
    void DoRun(const P::Message& m);
    void DoLiveStart(const P::Message& m);
    void DoLiveStop(const P::Message& m);
    void DoPause(const P::Message& m);
    void DoStep(const P::Message& m);
    void DoInput(const P::Message& m);
    void DoInputMulti(const P::Message& m);
    void DoRead(const P::Message& m);
    void DoWrite(const P::Message& m);
    void DoRegisters(const P::Message& m);
    void DoSetRegister(const P::Message& m);
    void DoScreenshot(const P::Message& m);
    void DoMemorySearch(const P::Message& m);
    void DoTextFind(const P::Message& m);
    void DoSubscribe(const P::Message& m);
    void DoUnsubscribe(const P::Message& m);

    bool RequireStopped(Instance& inst, int id, uint32_t seq);
    bool RequireBoundary(Instance& inst, int id, uint32_t seq);
    void DoBreakpointAdd(const P::Message& m);
    void DoWatchpointAdd(const P::Message& m);
    void DoPointRemove(const P::Message& m);
    void DoPointEnable(const P::Message& m);
    void DoPointList(const P::Message& m);
    void DoSavestateSave(const P::Message& m);
    void DoSavestateLoad(const P::Message& m);
    void DoLuaRun(const P::Message& m);
    void DoLuaExec(const P::Message& m);
    void DoLuaStop(const P::Message& m);
    void DoTraceConfig(const P::Message& m);
    void DoTraceRead(const P::Message& m);
    void DoCallsTrack(const P::Message& m);
    void DoCallStack(const P::Message& m);
    void DoCallCountsRead(const P::Message& m);
    void DoCoverageTrack(const P::Message& m);
    void DoCoverageRead(const P::Message& m);
    void DoWritersTrack(const P::Message& m);
    void DoLastWriter(const P::Message& m);
    void DoPacketsTrack(const P::Message& m);
    void DoPacketsRead(const P::Message& m);
    void DoInputLogRead(const P::Message& m);
    void DoSymbolsLoad(const P::Message& m);
    void DoPacing(const P::Message& m);
    void DoMpTimeout(const P::Message& m);
};

Server* TheServer = nullptr;
std::mutex TheServerLock;

// ----------------------------------------------------------------------------
// Start / stop

bool Server::Start(uint32_t address, int port, int range, int& bound, std::string& error)
{
    if (Running())
    {
        error = "server already running";
        return false;
    }
    uint16_t actual = 0;
    uint32_t span = port == 0 ? 1 : (range <= 0 ? 32 : std::min(range, 65535));
    Listener = Net::BindFirstFree(uint16_t(port), uint16_t(span), actual, address);
    if (Listener == Net::InvalidSocket)
    {
        error = "no free port in " + std::to_string(port) + "-" + std::to_string(std::min<uint32_t>(65535, port + span - 1));
        return false;
    }
    Net::SetNonBlocking(Listener, true);
    Port = actual;
    bound = actual;

    Quit = false;
    {
        std::lock_guard lock(ShutdownLock);
        ShutdownRequested = false;
    }
    ResetOptions();
    Out.Clear();
    Subscriptions::SetSink([this](int, std::vector<uint8_t>&& msg) {
        if (Connected)
            Out.PushFrame(std::move(msg));
    });
    Api::SetConsoleHooks([this](Instance& inst) { ApplyOptions(inst); }, [this](int id) {
        if (Connected)
            Out.PushReliable(P::Pack(uint16_t(id), P::ConsoleGoneEvent{}));
    });
    Thread = std::thread([this] { Main(); });
    return true;
}

void Server::Stop()
{
    if (!Running())
        return;
    Quit = true;
    Thread.join();
    Api::SetConsoleHooks({}, {});
    Subscriptions::SetSink({});
    Net::CloseSocket(Listener);
    Listener = Net::InvalidSocket;
}

int Server::Wait(int timeoutMs)
{
    std::unique_lock lock(ShutdownLock);
    if (timeoutMs < 0)
        ShutdownCond.wait(lock, [this] { return ShutdownRequested; });
    else
        ShutdownCond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return ShutdownRequested; });
    return ShutdownRequested ? 1 : 0;
}

// ----------------------------------------------------------------------------
// Options

void Server::ResetOptions()
{
    {
        std::lock_guard lock(OptLock);
        Mode = EmptyMode::Repeat;
        Stall = DefaultStall;
        Pinned = -1;
    }
    for (int id : Api::ConsoleIds())
        if (Instance* inst = Api::FindInstance(id))
        {
            inst->Input.SetMode(EmptyMode::Repeat);
            inst->Input.SetStallTimeout(DefaultStall);
        }
}

void Server::ApplyOptions(Instance& inst)
{
    EmptyMode mode;
    int stall;
    {
        std::lock_guard lock(OptLock);
        mode = Mode;
        stall = Stall;
    }
    inst.Input.SetMode(mode);
    inst.Input.SetStallTimeout(stall);
}

// ----------------------------------------------------------------------------
// Connection handling

void Server::Main()
{
    while (!Quit)
    {
        if (Client == Net::InvalidSocket)
        {
            if (Net::WaitReadable(Listener, 20))
                AcceptOne();
            continue;
        }

        bool readable = Net::WaitReadable(Client, PollMs);
        if (Net::WaitReadable(Listener, 0))
            AcceptOne();
        if (readable && !ReadClient())
        {
            Disconnect();
            continue;
        }
        ServiceBreaks();
        ServiceLua();
        ServicePending();
        if (!Flush())
        {
            Disconnect();
            continue;
        }
        if (CloseAfterFlush && ((Cur.empty() && Out.Empty()) || std::chrono::steady_clock::now() > CloseDeadline))
            Disconnect();
    }
    if (Client != Net::InvalidSocket)
        Disconnect();
}

void Server::AcceptOne()
{
    Net::Socket s = Net::Accept(Listener);
    if (s == Net::InvalidSocket)
        return;
    if (Client == Net::InvalidSocket)
        BeginClient(s);
    else
        RejectBusy(s);
}

void Server::RejectBusy(Net::Socket s)
{
    P::ErrorReply e;
    e.Code = P::ErrorCode::busy;
    e.Text = "another client is connected";
    auto msg = P::Pack(P::DefaultConsole, e);
    Net::SendAll(s, msg.data(), msg.size());
    // Read what the client already sent so closing does not reset the connection under our reply.
    uint8_t scratch[512];
    for (int i = 0; i < 8 && Net::WaitReadable(s, 25); i++)
        if (Net::Recv(s, scratch, sizeof scratch) <= 0)
            break;
    Net::CloseSocket(s);
}

void Server::BeginClient(Net::Socket s)
{
    Net::SetNonBlocking(s, true);
    Client = s;
    Parser = P::FrameParser();
    Out.Clear();
    Cur.clear();
    CurOff = 0;
    Pending.clear();
    Started.clear();
    CloseAfterFlush = false;
    ResetOptions();
    for (int id : Api::ConsoleIds())     // breaks from before this client are not news
    {
        mds_break stale;
        mds_take_break(id, &stale);
        if (Instance* inst = Api::FindInstance(id))
            if (inst->Lua)
                inst->Lua->TakeOutput();
    }
    Connected = true;
}

void Server::Disconnect()
{
    Connected = false;
    Net::CloseSocket(Client);
    Client = Net::InvalidSocket;
    if (!Cur.empty())
        Out.Release(Cur.size());
    Cur.clear();
    CurOff = 0;
    Out.Clear();
    Pending.clear();
    for (int id : Api::ConsoleIds())
        if (Instance* inst = Api::FindInstance(id))
        {
            inst->Subs.Clear();
            inst->Input.Clear();
        }
    for (int id : Started)
        if (Instance* inst = Api::FindInstance(id))
            inst->StopAfter(0);
    Started.clear();
    ResetOptions();
}

bool Server::ReadClient()
{
    uint8_t buf[65536];
    for (;;)
    {
        int n = Net::Recv(Client, buf, sizeof buf);
        if (n > 0)
        {
            if (!Parser.Feed(buf, size_t(n)))
            {
                Error(P::DefaultConsole, 0, P::ErrorCode::bad_message, "oversize message");
                CloseAfterFlush = true;
                CloseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                return true;
            }
            continue;
        }
        if (n == 0)
            return false;
        if (Net::WouldBlock())
            break;
        return false;
    }
    P::Message m;
    while (!CloseAfterFlush && Parser.Next(m))
        Handle(m);
    return true;
}

bool Server::Flush()
{
    for (;;)
    {
        if (Cur.empty())
        {
            CurOff = 0;
            if (!Out.Take(Cur))
                return true;
        }
        int n = Net::Send(Client, Cur.data() + CurOff, Cur.size() - CurOff);
        if (n > 0)
        {
            CurOff += size_t(n);
            if (CurOff == Cur.size())
            {
                Out.Release(Cur.size());
                Cur.clear();
            }
            continue;
        }
        if (n < 0 && Net::WouldBlock())
            return true;
        return false;
    }
}

// A console that parks sends a break event; the event goes out before any reply that
// reports the console stopped.
void Server::SendBreaks(int id)
{
    mds_break b;
    while (mds_take_break(id, &b) == 1)
    {
        P::BreakEvent e;
        e.Kind = b.kind;
        e.Cpu = b.cpu;
        e.Size = b.size;
        e.Id = b.id;
        e.Pc = b.pc;
        e.Addr = b.addr;
        e.Value = b.value;
        e.Dma = b.dma;
        Reply(uint16_t(id), e);
    }
}

void Server::ServiceBreaks()
{
    for (int id : Api::ConsoleIds())
        SendBreaks(id);
}

// run, step, pause and live_stop reply once their consoles have stopped (idle or parked).
// When a console parks during a run, the others of that run are interrupted, as mds_wait does.
void Server::ServicePending()
{
    for (size_t i = 0; i < Pending.size();)
    {
        PendingOp& op = Pending[i];
        bool running = false, parked = false;
        for (int id : op.Ids)
            if (Instance* inst = Api::FindInstance(id))
            {
                RunState st = inst->GetState();
                running |= st == RunState::Running;
                parked |= st == RunState::Parked;
            }
        if (op.Kind == PendingKind::Run && parked && running && !op.Interrupted)
        {
            for (int id : op.Ids)
                if (Instance* inst = Api::FindInstance(id))
                    if (inst->GetState() == RunState::Running)
                        inst->Interrupt();
            op.Interrupted = true;
        }
        if (running)
        {
            i++;
            continue;
        }
        for (int id : op.Ids)
        {
            SendBreaks(id);
            SendLuaOutput(id);
        }
        Ok(op.Console, op.Seq);
        Pending.erase(Pending.begin() + i);
    }
}

// ----------------------------------------------------------------------------
// Messages

bool Server::Resolve(const P::Message& m, int& id, Instance*& inst)
{
    P::ErrorCode code = P::ResolveConsole(m.Head.Console, [&] { std::lock_guard lock(OptLock); return Pinned; }(),
                                          Api::ConsoleIds(), id);
    inst = code == P::ErrorCode::none ? Api::FindInstance(id) : nullptr;
    if (inst)
        return true;
    Error(m.Head.Console, Le32(m.Body), P::ErrorCode::no_console,
          m.Head.Console == P::DefaultConsole ? "no default console" : "no console " + std::to_string(m.Head.Console));
    return false;
}

bool Server::ResolveList(const P::Message& m, uint32_t seq, const std::vector<uint16_t>& wanted, std::vector<int>& out)
{
    out.clear();
    if (wanted.empty())
        out = Api::ConsoleIds();
    else
        for (uint16_t id : wanted)
        {
            if (!Api::FindInstance(id))
            {
                Error(m.Head.Console, seq, P::ErrorCode::no_console, "no console " + std::to_string(id));
                return false;
            }
            out.push_back(id);
        }
    return true;
}

void Server::DoHello(const P::Message& m)
{
    P::Hello h;
    if (!Parse(m, h))
        return;
    if (h.ProtocolVersion != P::ProtocolVersion)
    {
        Error(m.Head.Console, h.Seq, P::ErrorCode::bad_payload,
              "unsupported protocol version " + std::to_string(h.ProtocolVersion));
        return;
    }
    if (h.EmptyQueue > 2)
    {
        Error(m.Head.Console, h.Seq, P::ErrorCode::bad_option, "unknown emptyQueue mode");
        return;
    }
    std::vector<int> ids = Api::ConsoleIds();
    int pinned = -1;
    if (h.DefaultConsole != P::DefaultConsole)
    {
        if (std::find(ids.begin(), ids.end(), int(h.DefaultConsole)) == ids.end())
        {
            Error(m.Head.Console, h.Seq, P::ErrorCode::no_console, "no console " + std::to_string(h.DefaultConsole));
            return;
        }
        pinned = h.DefaultConsole;
    }
    {
        std::lock_guard lock(OptLock);
        Mode = EmptyMode(h.EmptyQueue);
        Stall = h.StallTimeoutFrames;
        Pinned = pinned;
    }
    for (int id : ids)
        if (Instance* inst = Api::FindInstance(id))
            ApplyOptions(*inst);

    P::HelloReply r;
    r.Seq = h.Seq;
    {
        std::lock_guard lock(NameLock);
        r.Name = Name;
    }
    r.Pid = uint32_t(GETPID());
    r.Port = Port;
    int resolved = 0;
    r.DefaultConsole = P::ResolveConsole(P::DefaultConsole, pinned, ids, resolved) == P::ErrorCode::none
        ? uint16_t(resolved) : P::DefaultConsole;
    for (int id : ids)
        r.Ids.push_back(uint16_t(id));
    Reply(P::DefaultConsole, r);
}

void Server::SendInputQueue(int id, size_t depth)
{
    P::InputQueueEvent e;
    e.Depth = uint16_t(std::min<size_t>(depth, 0xFFFF));
    Reply(uint16_t(id), e);
}

void Server::DoSetOption(const P::Message& m)
{
    P::SetOption o;
    if (!Parse(m, o))
        return;
    uint16_t hdr = m.Head.Console;
    auto bad = [&](const char* text) { Error(hdr, o.Seq, P::ErrorCode::bad_option, text); };

    if (o.Key == P::OptDefaultConsole)
    {
        if (o.Value != P::DefaultConsole && (o.Value > 0xFFFF || !Api::FindInstance(int(o.Value))))
            return bad("no such console");
        std::lock_guard lock(OptLock);
        Pinned = o.Value == P::DefaultConsole ? -1 : int(o.Value);
        Ok(hdr, o.Seq);
        return;
    }
    if (o.Key != P::OptEmptyQueue && o.Key != P::OptStallTimeoutFrames)
        return bad("unknown option");
    if (o.Key == P::OptEmptyQueue ? o.Value > 2 : o.Value > 0xFFFF)
        return bad("value out of range");

    std::vector<int> ids;
    if (hdr == P::DefaultConsole)
        ids = Api::ConsoleIds();
    else if (Api::FindInstance(hdr))
        ids.push_back(hdr);
    else
        return Error(hdr, o.Seq, P::ErrorCode::no_console, "no console " + std::to_string(hdr));
    for (int id : ids)
        if (Instance* inst = Api::FindInstance(id))
        {
            if (o.Key == P::OptEmptyQueue)
                inst->Input.SetMode(EmptyMode(o.Value));
            else
                inst->Input.SetStallTimeout(int(o.Value));
        }
    Ok(hdr, o.Seq);
}

void Server::DoConsoleCreate(const P::Message& m)
{
    P::ConsoleCreate c;
    if (!Parse(m, c))
        return;
    int id = mds_create(c.Bios9.c_str(), c.Bios7.c_str(), c.Firmware.c_str(), c.Nickname.c_str(), c.Debug);
    if (id < 0)
        return FailFromApi(m.Head.Console, c.Seq);
    P::ConsoleCreateReply r;
    r.Seq = c.Seq;
    r.Id = uint16_t(id);
    Reply(uint16_t(id), r);
}

void Server::DoConsoleDestroy(const P::Message& m)
{
    P::ConsoleDestroy d;
    int id;
    Instance* inst;
    if (!Parse(m, d) || !Resolve(m, id, inst))
        return;
    mds_destroy(id);        // the destroy hook sends console_gone
    Ok(uint16_t(id), d.Seq);
}

void Server::DoConsoleList(const P::Message& m)
{
    P::ConsoleList l;
    if (!Parse(m, l))
        return;
    P::ConsoleListReply r;
    r.Seq = l.Seq;
    for (int id : Api::ConsoleIds())
        if (Instance* inst = Api::FindInstance(id))
        {
            P::ConsoleInfo info;
            info.Id = uint16_t(id);
            info.State = uint8_t(inst->GetState());
            info.Frames = inst->FramesRun;
            r.Consoles.push_back(info);
        }
    Reply(P::DefaultConsole, r);
}

void Server::DoRomLoad(const P::Message& m)
{
    P::RomLoad r;
    int id;
    Instance* inst;
    if (!Parse(m, r) || !Resolve(m, id, inst))
        return;
    if (mds_load_rom(id, r.Rom.c_str(), r.Save.empty() ? nullptr : r.Save.c_str()) < 0)
        return FailFromApi(uint16_t(id), r.Seq);
    SendInputQueue(id, inst->Input.Depth());
    Ok(uint16_t(id), r.Seq);
}

void Server::DoReset(const P::Message& m)
{
    P::Reset r;
    int id;
    Instance* inst;
    if (!Parse(m, r) || !Resolve(m, id, inst))
        return;
    if (mds_reset(id) < 0)
        return FailFromApi(uint16_t(id), r.Seq);
    SendInputQueue(id, inst->Input.Depth());
    Ok(uint16_t(id), r.Seq);
}

void Server::DoShutdown(const P::Message& m)
{
    P::Shutdown s;
    if (!Parse(m, s))
        return;
    Ok(m.Head.Console, s.Seq);
    {
        std::lock_guard lock(ShutdownLock);
        ShutdownRequested = true;
    }
    ShutdownCond.notify_all();
    CloseAfterFlush = true;
    CloseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

// Start every console for `frames`; none is started if one is already running.
bool Server::StartAll(const P::Message& m, uint32_t seq, const std::vector<int>& ids, uint32_t frames)
{
    for (int id : ids)
        if (Api::FindInstance(id)->GetState() == RunState::Running)
        {
            Error(m.Head.Console, seq, P::ErrorCode::console_running, "console " + std::to_string(id) + " is running");
            return false;
        }
    for (int id : ids)
    {
        Api::FindInstance(id)->Start(frames);
        Started.insert(id);
    }
    return true;
}

void Server::DoRun(const P::Message& m)
{
    P::Run r;
    std::vector<int> ids;
    if (!Parse(m, r) || !ResolveList(m, r.Seq, r.Consoles, ids) || !StartAll(m, r.Seq, ids, r.Frames))
        return;
    Pending.push_back({PendingKind::Run, r.Seq, m.Head.Console, ids});
}

void Server::DoLiveStart(const P::Message& m)
{
    P::LiveStart l;
    std::vector<int> ids;
    if (!Parse(m, l) || !ResolveList(m, l.Seq, l.Consoles, ids))
        return;
    if (!StartAll(m, l.Seq, ids, 0xFFFFFFFFu))
        return;
    mds_set_pacing(int(std::min<uint32_t>(l.Fps, 1000000)));
    Ok(m.Head.Console, l.Seq);
}

void Server::DoLiveStop(const P::Message& m)
{
    P::LiveStop l;
    std::vector<int> ids;
    if (!Parse(m, l) || !ResolveList(m, l.Seq, l.Consoles, ids))
        return;
    for (int id : ids)
        Api::FindInstance(id)->StopAfter(0);
    Pending.push_back({PendingKind::Stop, l.Seq, m.Head.Console, ids});
}

void Server::DoPause(const P::Message& m)
{
    P::Pause p;
    int id;
    Instance* inst;
    if (!Parse(m, p) || !Resolve(m, id, inst))
        return;
    inst->StopAfter(0);
    Pending.push_back({PendingKind::Stop, p.Seq, uint16_t(id), {id}});
}

// Arms the step, then runs the console until it parks on it (or StepFrameCap frames pass).
void Server::DoStep(const P::Message& m)
{
    P::Step s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(s.Cpu))
        return Error(uint16_t(id), s.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    if (inst->GetState() == RunState::Running)
        return Error(uint16_t(id), s.Seq, P::ErrorCode::console_running, "console " + std::to_string(id) + " is running");
    if (mds_step(id, s.Cpu, s.Count) < 0)
        return FailFromApi(uint16_t(id), s.Seq);
    inst->Start(StepFrameCap);
    Started.insert(id);
    Pending.push_back({PendingKind::Run, s.Seq, uint16_t(id), {id}});
}

Headless::InputPayload ToPayload(P::InputPayload& in)
{
    Headless::InputPayload p;
    p.Source = InputSource::Client;
    p.Seq = in.Seq;
    p.Buttons = in.Buttons;
    p.Lid = in.Lid;
    for (const P::TouchPoint& t : in.Touch)
        p.Touch.push_back({t.Offset, t.X, t.Y, t.Down});
    p.MicKind = in.MicKind;
    p.Mic = std::move(in.Mic);
    p.MicLevel = in.MicLevel;
    return p;
}

// All entries are checked before any is queued: an unknown console, a bad payload or a full queue fails the whole
// message and leaves every queue as it was.
void Server::DoInputMulti(const P::Message& m)
{
    P::InputMulti req;
    if (!Parse(m, req))
        return;
    uint16_t head = m.Head.Console;
    if (req.Entries.empty())
        return Error(head, req.Seq, P::ErrorCode::bad_payload, "input_multi needs at least one entry");
    std::vector<GroupEntry> entries;
    for (P::InputMultiEntry& e : req.Entries)
    {
        Instance* inst = Api::FindInstance(e.Console);
        if (!inst)
            return Error(head, req.Seq, P::ErrorCode::no_console, "no console " + std::to_string(e.Console));
        if (!P::ValidatePayload(e.Payload))
            return Error(head, req.Seq, P::ErrorCode::bad_payload,
                         "invalid input payload for console " + std::to_string(e.Console));
        GroupEntry g;
        g.Queue = &inst->Input;
        g.Console = e.Console;
        g.Payload = ToPayload(e.Payload);
        entries.push_back(std::move(g));
    }
    std::vector<int> depths;
    std::string error;
    if (!SubmitGroup(entries, depths, error))
        return Error(head, req.Seq, MapError(error), error);
    P::InputMultiReply r;
    r.Seq = req.Seq;
    for (int d : depths)
        r.Depths.push_back(uint16_t(std::min(d, 0xFFFF)));
    Reply(head, r);
}

void Server::DoInput(const P::Message& m)
{
    P::InputPayload in;
    int id;
    Instance* inst;
    if (!Parse(m, in) || !Resolve(m, id, inst))
        return;
    if (!P::ValidatePayload(in))
        return Error(uint16_t(id), in.Seq, P::ErrorCode::bad_payload, "invalid input payload");
    Headless::InputPayload p = ToPayload(in);
    std::string error;
    int depth = inst->Input.Submit(std::move(p), error);
    if (depth < 0)
        return Error(uint16_t(id), in.Seq, MapError(error), error);
    P::InputReply r;
    r.Seq = in.Seq;
    r.Depth = uint16_t(std::min(depth, 0xFFFF));
    Reply(uint16_t(id), r);
}

void Server::DoRead(const P::Message& m)
{
    P::ReadReq r;
    int id;
    Instance* inst;
    if (!Parse(m, r) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(r.Cpu) || r.Len > P::MaxBody - 8)
        return Error(uint16_t(id), r.Seq, P::ErrorCode::bad_payload, "bad cpu or length");
    P::BytesReply reply;
    reply.Seq = r.Seq;
    if (!CopyRange(*inst, r.Cpu, r.Addr, r.Len, reply.Data))
        return Error(uint16_t(id), r.Seq, P::ErrorCode::no_console, "console is shutting down");
    Reply(uint16_t(id), reply);
}

void Server::DoWrite(const P::Message& m)
{
    P::WriteReq w;
    int id;
    Instance* inst;
    if (!Parse(m, w) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(w.Cpu))
        return Error(uint16_t(id), w.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    if (mds_write(id, w.Cpu, w.Addr, uint32_t(w.Data.size()), w.Data.data()) < 0)
        return FailFromApi(uint16_t(id), w.Seq);
    Ok(uint16_t(id), w.Seq);
}

void Server::DoRegisters(const P::Message& m)
{
    P::RegistersReq r;
    int id;
    Instance* inst;
    if (!Parse(m, r) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(r.Cpu))
        return Error(uint16_t(id), r.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    uint32_t regs[19];
    if (mds_get_regs(id, r.Cpu, regs) < 0)
        return FailFromApi(uint16_t(id), r.Seq);
    P::RegistersReply reply;
    reply.Seq = r.Seq;
    reply.Regs.assign(regs, regs + 19);
    Reply(uint16_t(id), reply);
}

void Server::DoSetRegister(const P::Message& m)
{
    P::SetRegister s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(s.Cpu))
        return Error(uint16_t(id), s.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    if (mds_set_reg(id, s.Cpu, s.Reg, s.Value) < 0)
        return FailFromApi(uint16_t(id), s.Seq);
    Ok(uint16_t(id), s.Seq);
}

void Server::DoScreenshot(const P::Message& m)
{
    P::Screenshot s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst))
        return;
    P::ScreenshotReply reply;
    reply.Seq = s.Seq;
    reply.Pixels.resize(P::ScreenshotBytes);
    bool have = false;
    bool ran = inst->Call([&](Instance& i) {
        void* top = nullptr;
        void* bottom = nullptr;
        if (!i.NDS->GPU.GetFramebuffers(&top, &bottom) || !top || !bottom)
            return;
        std::memcpy(reply.Pixels.data(), top, P::ScreenshotBytes / 2);
        std::memcpy(reply.Pixels.data() + P::ScreenshotBytes / 2, bottom, P::ScreenshotBytes / 2);
        reply.Frame = i.FramesRun;
        have = true;
    });
    if (!ran)
        return Error(uint16_t(id), s.Seq, P::ErrorCode::no_console, "console is shutting down");
    if (!have)
        return Error(uint16_t(id), s.Seq, P::ErrorCode::failed, "no framebuffer");
    Reply(uint16_t(id), reply);
}

bool Server::CopyRange(Instance& inst, int cpu, uint32_t start, uint32_t len, std::vector<uint8_t>& out)
{
    out.resize(len);
    return inst.Call([&](Instance& i) { Api::ReadGuest(i, cpu, start, len, out.data()); });
}

bool Server::SearchRange(const P::Message& m, uint32_t seq, uint32_t start0, uint32_t len0, uint32_t& start, uint32_t& len)
{
    start = P::ResolveStart(start0, len0);
    len = P::ResolveLength(start0, len0);
    if (uint64_t(start) + len > 0x100000000ull || len > 0x1000000)
    {
        Error(m.Head.Console, seq, P::ErrorCode::bad_payload, "search range too large");
        return false;
    }
    return true;
}

void Server::DoMemorySearch(const P::Message& m)
{
    P::MemorySearch s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst))
        return;
    uint32_t start, len;
    if (!P::ValidateSearch(s) || !CpuOk(s.Cpu))
        return Error(uint16_t(id), s.Seq, P::ErrorCode::bad_payload, "bad search request");
    if (!SearchRange(m, s.Seq, s.Start, s.Length, start, len))
        return;
    std::vector<uint8_t> mem;
    if (!CopyRange(*inst, s.Cpu, start, len, mem))
        return Error(uint16_t(id), s.Seq, P::ErrorCode::no_console, "console is shutting down");
    Search::ValueHits hits = Search::FindValue(mem.data(), mem.size(), start, s.Size, s.Needle.data(), s.Needle.size(),
                                               P::ResolveSearchLimit(s.Limit, P::DefaultMemorySearchLimit));
    P::MemorySearchReply r;
    r.Seq = s.Seq;
    r.Truncated = hits.Truncated;
    r.Addrs = std::move(hits.Addrs);
    Namer(*inst, s.WithSymbols).Fill(r.Addrs, r.HasNames, r.Names);
    Reply(uint16_t(id), r);
}

void Server::DoTextFind(const P::Message& m)
{
    P::TextFind t;
    int id;
    Instance* inst;
    if (!Parse(m, t) || !Resolve(m, id, inst))
        return;
    uint32_t start, len;
    std::vector<Search::EncodedText> patterns;
    if (!CpuOk(t.Cpu) || !Search::EncodeText(t.Text, P::ResolveEncodings(t.Encodings), patterns))
        return Error(uint16_t(id), t.Seq, P::ErrorCode::bad_payload, "bad cpu or text is not valid UTF-8");
    if (!SearchRange(m, t.Seq, t.Start, t.Length, start, len))
        return;
    P::TextFindReply r;
    r.Seq = t.Seq;
    if (!patterns.empty())
    {
        std::vector<uint8_t> mem;
        if (!CopyRange(*inst, t.Cpu, start, len, mem))
            return Error(uint16_t(id), t.Seq, P::ErrorCode::no_console, "console is shutting down");
        Search::TextHits hits = Search::FindText(mem.data(), mem.size(), start, patterns,
                                                 P::ResolveSearchLimit(t.Limit, P::DefaultTextFindLimit));
        r.Truncated = hits.Truncated;
        for (const Search::TextHit& h : hits.Hits)
            r.Hits.push_back({h.Addr, h.Encoding});
    }
    std::vector<uint32_t> hitAddrs;
    for (const P::TextHit& h : r.Hits)
        hitAddrs.push_back(h.Addr);
    Namer(*inst, t.WithSymbols).Fill(hitAddrs, r.HasNames, r.Names);
    Reply(uint16_t(id), r);
}

void Server::DoSubscribe(const P::Message& m)
{
    P::Subscribe s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst))
        return;
    std::vector<SubRangeSpec> ranges;
    for (const P::SubRange& r : s.Ranges)
    {
        if (!CpuOk(r.Cpu) || r.Len == 0)
            return Error(uint16_t(id), s.Seq, P::ErrorCode::bad_payload, "bad subscription range");
        ranges.push_back({r.Cpu, r.Addr, r.Len});
    }
    if (!inst->Subs.Set(s.Id, s.EveryNFrames, std::move(ranges)))
        return Error(uint16_t(id), s.Seq, P::ErrorCode::bad_payload, "subscriptions too large for one event");
    Ok(uint16_t(id), s.Seq);
}

void Server::DoUnsubscribe(const P::Message& m)
{
    P::Unsubscribe u;
    int id;
    Instance* inst;
    if (!Parse(m, u) || !Resolve(m, id, inst))
        return;
    if (!inst->Subs.Remove(u.Value))
        return Error(uint16_t(id), u.Seq, P::ErrorCode::failed, "no such subscription");
    Ok(uint16_t(id), u.Seq);
}

// ----------------------------------------------------------------------------
// Debug and analysis messages

constexpr int MaxListedPoints = 4096;
constexpr int MaxCallFrames = 16384;
constexpr uint32_t MaxTraceCapacity = 1u << 24;
constexpr uint32_t MaxPacketCapacity = 1u << 20;
constexpr int MaxPacketBytes = 4 << 20;

// The state check for messages the C API cannot answer by itself (it reports a console it cannot use as an error).
bool Server::RequireStopped(Instance& inst, int id, uint32_t seq)
{
    if (inst.GetState() != RunState::Running)
        return true;
    Error(uint16_t(id), seq, P::ErrorCode::console_running, "console " + std::to_string(id) + " is running");
    return false;
}

void Server::DoBreakpointAdd(const P::Message& m)
{
    P::BreakpointAdd b;
    int id;
    Instance* inst;
    if (!Parse(m, b) || !Resolve(m, id, inst))
        return;
    if (b.CpuMask == 0 || (b.CpuMask & ~3u))
        return Error(uint16_t(id), b.Seq, P::ErrorCode::bad_payload, "cpuMask must be 1 (ARM9), 2 (ARM7) or 3");
    int point = mds_bp_add(id, b.CpuMask, b.Addr);
    if (point < 0)
        return FailFromApi(uint16_t(id), b.Seq);
    P::IdReply r;
    r.Seq = b.Seq;
    r.Id = point;
    Reply(uint16_t(id), r);
}

void Server::DoWatchpointAdd(const P::Message& m)
{
    P::WatchpointAdd w;
    int id;
    Instance* inst;
    if (!Parse(m, w) || !Resolve(m, id, inst))
        return;
    if (w.CpuMask == 0 || (w.CpuMask & ~3u) || w.Size == 0 || w.Flags == 0 || (w.Flags & ~3u))
        return Error(uint16_t(id), w.Seq, P::ErrorCode::bad_payload,
                     "watchpoint needs cpuMask 1-3, a size, and flags 1 (read), 2 (write) or 3");
    int point = mds_wp_add(id, w.CpuMask, w.Addr, w.Size, w.Flags);
    if (point < 0)
        return FailFromApi(uint16_t(id), w.Seq);
    P::IdReply r;
    r.Seq = w.Seq;
    r.Id = point;
    Reply(uint16_t(id), r);
}

void Server::DoPointRemove(const P::Message& m)
{
    P::PointRemove p;
    int id;
    Instance* inst;
    if (!Parse(m, p) || !Resolve(m, id, inst))
        return;
    if (mds_point_remove(id, p.Id) < 0)
        return FailFromApi(uint16_t(id), p.Seq);
    Ok(uint16_t(id), p.Seq);
}

void Server::DoPointEnable(const P::Message& m)
{
    P::PointEnable p;
    int id;
    Instance* inst;
    if (!Parse(m, p) || !Resolve(m, id, inst))
        return;
    if (mds_point_enable(id, p.Id, p.Enabled) < 0)
        return FailFromApi(uint16_t(id), p.Seq);
    Ok(uint16_t(id), p.Seq);
}

void Server::DoPointList(const P::Message& m)
{
    P::PointList p;
    int id;
    Instance* inst;
    if (!Parse(m, p) || !Resolve(m, id, inst))
        return;
    std::vector<mds_point> points(MaxListedPoints);
    int n = mds_point_list(id, points.data(), MaxListedPoints);
    if (n < 0)
        return FailFromApi(uint16_t(id), p.Seq);
    P::PointListReply r;
    r.Seq = p.Seq;
    for (int i = 0; i < n; i++)
    {
        const mds_point& q = points[i];
        r.Points.push_back({q.id, uint8_t(q.watch ? 1 : 0), uint8_t(q.enabled ? 1 : 0), q.cpumask, q.flags, q.start,
                            q.end, q.hits});
    }
    Reply(uint16_t(id), r);
}

// Savestates need a frame boundary. A running console reaches one by itself but the C API refuses it; a console
// parked mid-frame would have to resume first. Both get console_running; waiting for the boundary on the
// client's behalf (as the Qt DebugBridge does) would silently run guest code and could hit further breaks.
bool Server::RequireBoundary(Instance& inst, int id, uint32_t seq)
{
    switch (inst.GetState())
    {
    case RunState::Idle:
        return true;
    case RunState::Running:
        Error(uint16_t(id), seq, P::ErrorCode::console_running,
              "console " + std::to_string(id) + " is running: stop it first (live_stop or pause)");
        return false;
    case RunState::Parked:
        Error(uint16_t(id), seq, P::ErrorCode::console_running,
              "console " + std::to_string(id) + " is stopped inside a frame (at a break or step); savestates need a "
              "frame boundary: resume it and let the frame finish first");
        return false;
    }
    return false;
}

void Server::DoSavestateSave(const P::Message& m)
{
    P::SavestateSave s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst) || !RequireBoundary(*inst, id, s.Seq))
        return;
    if (mds_savestate(id, s.Text.c_str()) < 0)
        return FailFromApi(uint16_t(id), s.Seq);
    Ok(uint16_t(id), s.Seq);
}

void Server::DoSavestateLoad(const P::Message& m)
{
    P::SavestateLoad s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst) || !RequireBoundary(*inst, id, s.Seq))
        return;
    if (mds_loadstate(id, s.Text.c_str()) < 0)
        return FailFromApi(uint16_t(id), s.Seq);
    SendInputQueue(id, inst->Input.Depth());      // the load cleared the input queue
    Ok(uint16_t(id), s.Seq);
}

// ---- Lua. The script's print output reaches the client as lua_output events, ahead of the reply.

void Server::SendLuaOutput(int id)
{
    Instance* inst = Api::FindInstance(id);
    if (!inst || !inst->Lua)
        return;
    std::string text = inst->Lua->TakeOutput();
    for (size_t pos = 0; pos < text.size();)
    {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end + 1;
        P::LuaOutputEvent e;
        e.Text = text.substr(pos, end - pos);
        Reply(uint16_t(id), e);
        pos = end;
    }
}

void Server::ServiceLua()
{
    for (int id : Api::ConsoleIds())
        SendLuaOutput(id);
}

void Server::DoLuaRun(const P::Message& m)
{
    P::LuaRun l;
    int id;
    Instance* inst;
    if (!Parse(m, l) || !Resolve(m, id, inst))
        return;
    int rc = mds_lua_run(id, l.Source.c_str(), l.IsFile);
    SendLuaOutput(id);
    if (rc < 0)
        return FailFromApi(uint16_t(id), l.Seq);
    Ok(uint16_t(id), l.Seq);
}

void Server::DoLuaExec(const P::Message& m)
{
    P::LuaExec l;
    int id;
    Instance* inst;
    if (!Parse(m, l) || !Resolve(m, id, inst))
        return;
    int rc = mds_lua_exec(id, l.Text.c_str());
    SendLuaOutput(id);
    if (rc < 0)
        return FailFromApi(uint16_t(id), l.Seq);
    Ok(uint16_t(id), l.Seq);
}

void Server::DoLuaStop(const P::Message& m)
{
    P::LuaStop l;
    int id;
    Instance* inst;
    if (!Parse(m, l) || !Resolve(m, id, inst))
        return;
    if (mds_lua_stop(id) < 0)
        return FailFromApi(uint16_t(id), l.Seq);
    Ok(uint16_t(id), l.Seq);
}

// ---- trace, calls, coverage, writers

void Server::DoTraceConfig(const P::Message& m)
{
    P::TraceConfig t;
    int id;
    Instance* inst;
    if (!Parse(m, t) || !Resolve(m, id, inst))
        return;
    if ((t.CpuMask & ~3u) || (t.Capacity && t.CpuMask == 0) || t.Capacity > MaxTraceCapacity)
        return Error(uint16_t(id), t.Seq, P::ErrorCode::bad_payload, "bad trace cpuMask or capacity");
    if (mds_trace(id, t.CpuMask, t.Start, t.End, t.Capacity) < 0)
        return FailFromApi(uint16_t(id), t.Seq);
    Ok(uint16_t(id), t.Seq);
}

void Server::DoTraceRead(const P::Message& m)
{
    P::TraceRead t;
    int id;
    Instance* inst;
    if (!Parse(m, t) || !Resolve(m, id, inst))
        return;
    int max = int(P::ResolveReadMax(t.Max));
    std::vector<mds_trace_entry> buf(max);
    int n = mds_trace_take(id, buf.data(), max);
    if (n < 0)
        return FailFromApi(uint16_t(id), t.Seq);
    P::TraceReadReply r;
    r.Seq = t.Seq;
    r.Truncated = Api::MoreStaged(id, Api::Staged::Trace, 9);
    std::vector<uint32_t> pcs;
    for (int i = 0; i < n; i++)
    {
        r.Entries.push_back({buf[i].cycle, uint8_t(buf[i].cpu), buf[i].pc, buf[i].instr, buf[i].cpsr});
        pcs.push_back(buf[i].pc);
    }
    Namer(*inst, t.WithSymbols).Fill(pcs, r.HasNames, r.Names);
    Reply(uint16_t(id), r);
}

void Server::DoCallsTrack(const P::Message& m)
{
    P::CallsTrack t;
    int id;
    Instance* inst;
    if (!Parse(m, t) || !Resolve(m, id, inst))
        return;
    if (mds_calls(id, t.Enabled) < 0)
        return FailFromApi(uint16_t(id), t.Seq);
    Ok(uint16_t(id), t.Seq);
}

void Server::DoCallStack(const P::Message& m)
{
    P::CallStackReq c;
    int id;
    Instance* inst;
    if (!Parse(m, c) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(c.Cpu))
        return Error(uint16_t(id), c.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    std::vector<mds_frame> frames(MaxCallFrames);
    int n = mds_callstack(id, c.Cpu, frames.data(), MaxCallFrames);
    if (n < 0)
        return FailFromApi(uint16_t(id), c.Seq);
    P::CallStackReply r;
    r.Seq = c.Seq;
    std::vector<uint32_t> entries, sites;
    for (int i = 0; i < n; i++)
    {
        const mds_frame& f = frames[i];
        r.Frames.push_back({f.site, f.entry, f.ret, f.sp, f.mode, uint8_t(f.exception ? 1 : 0)});
        entries.push_back(f.entry);
        sites.push_back(f.site);
    }
    Namer namer(*inst, c.WithSymbols);
    namer.Fill(entries, r.HasNames, r.Names);
    uint8_t has;
    namer.Fill(sites, has, r.SiteNames);
    Reply(uint16_t(id), r);
}

void Server::DoCallCountsRead(const P::Message& m)
{
    P::CallCountsRead c;
    int id;
    Instance* inst;
    if (!Parse(m, c) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(c.Cpu))
        return Error(uint16_t(id), c.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    int max = int(P::ResolveReadMax(c.Max));
    std::vector<uint32_t> entries(max);
    std::vector<uint64_t> counts(max);
    int n = mds_callcounts_take(id, c.Cpu, entries.data(), counts.data(), max);
    if (n < 0)
        return FailFromApi(uint16_t(id), c.Seq);
    entries.resize(n);
    P::CallCountsReply r;
    r.Seq = c.Seq;
    r.Truncated = Api::MoreStaged(id, Api::Staged::CallCounts, c.Cpu);
    for (int i = 0; i < n; i++)
        r.Counts.push_back({entries[i], counts[i]});
    Namer(*inst, c.WithSymbols).Fill(entries, r.HasNames, r.Names);
    Reply(uint16_t(id), r);
}

void Server::DoCoverageTrack(const P::Message& m)
{
    P::CoverageTrack t;
    int id;
    Instance* inst;
    if (!Parse(m, t) || !Resolve(m, id, inst))
        return;
    if (mds_coverage(id, t.Enabled) < 0)
        return FailFromApi(uint16_t(id), t.Seq);
    Ok(uint16_t(id), t.Seq);
}

void Server::DoCoverageRead(const P::Message& m)
{
    P::CoverageRead c;
    int id;
    Instance* inst;
    if (!Parse(m, c) || !Resolve(m, id, inst))
        return;
    if (!CpuOk(c.Cpu))
        return Error(uint16_t(id), c.Seq, P::ErrorCode::bad_payload, "cpu must be 9 or 7");
    int max = int(P::ResolveReadMax(c.Max));
    P::CoverageReadReply r;
    r.Seq = c.Seq;
    r.Addrs.resize(max);
    int n = mds_coverage_take(id, c.Cpu, r.Addrs.data(), max);
    if (n < 0)
        return FailFromApi(uint16_t(id), c.Seq);
    r.Addrs.resize(n);
    r.Truncated = Api::MoreStaged(id, Api::Staged::Coverage, c.Cpu);
    Namer(*inst, c.WithSymbols).Fill(r.Addrs, r.HasNames, r.Names);
    Reply(uint16_t(id), r);
}

void Server::DoWritersTrack(const P::Message& m)
{
    P::WritersTrack t;
    int id;
    Instance* inst;
    if (!Parse(m, t) || !Resolve(m, id, inst))
        return;
    if (mds_lastwriter(id, t.Enabled) < 0)
        return FailFromApi(uint16_t(id), t.Seq);
    Ok(uint16_t(id), t.Seq);
}

void Server::DoLastWriter(const P::Message& m)
{
    P::LastWriter l;
    int id;
    Instance* inst;
    if (!Parse(m, l) || !Resolve(m, id, inst) || !RequireStopped(*inst, id, l.Seq))
        return;
    // Debugger::GetLastWriter packs a CPU writer as pc | (arm7 ? 1 : 0), a DMA writer as
    // 0xFFFFFFF0 | channel << 1 | (arm7 ? 1 : 0), and no writer as 0.
    uint32_t raw = mds_lastwriter_get(id, l.Value);
    P::LastWriterReply r;
    r.Seq = l.Seq;
    r.Raw = raw;
    if (raw >= 0xFFFFFFF0u)
    {
        r.Kind = 2;
        r.Channel = uint8_t((raw >> 1) & 3);
        r.Cpu = (raw & 1) ? 7 : 9;
    }
    else if (raw)
    {
        r.Kind = 1;
        r.Pc = raw & ~1u;
        r.Cpu = (raw & 1) ? 7 : 9;
    }
    Reply(uint16_t(id), r);
}

// ---- packets (work while the console runs)

void Server::DoPacketsTrack(const P::Message& m)
{
    P::PacketsTrack p;
    int id;
    Instance* inst;
    if (!Parse(m, p) || !Resolve(m, id, inst))
        return;
    if (p.Capacity > MaxPacketCapacity)
        return Error(uint16_t(id), p.Seq, P::ErrorCode::bad_payload, "packet capacity too large");
    if (mds_packets(id, p.Enabled, int(p.Capacity)) < 0)
        return FailFromApi(uint16_t(id), p.Seq);
    Ok(uint16_t(id), p.Seq);
}

void Server::DoPacketsRead(const P::Message& m)
{
    P::PacketsRead p;
    int id;
    Instance* inst;
    if (!Parse(m, p) || !Resolve(m, id, inst))
        return;
    int max = int(P::ResolveReadMax(p.Max));
    std::vector<mds_packet> headers(max);
    std::vector<uint8_t> data(MaxPacketBytes);
    int n = mds_packets_take(id, headers.data(), data.data(), max, MaxPacketBytes);
    if (n < 0)
        return FailFromApi(uint16_t(id), p.Seq);
    P::PacketsReadReply r;
    r.Seq = p.Seq;
    r.Truncated = Api::MoreStaged(id, Api::Staged::Packets, 9);
    size_t off = 0;
    for (int i = 0; i < n; i++)
    {
        P::PacketItem item;
        item.Timestamp = headers[i].timestamp;
        item.Sent = headers[i].sent ? 1 : 0;
        item.Type = uint8_t(headers[i].type);
        item.Data.assign(data.begin() + off, data.begin() + off + headers[i].length);
        off += headers[i].length;
        r.Packets.push_back(std::move(item));
    }
    Reply(uint16_t(id), r);
}

// ---- input log, symbols, process-wide

void Server::DoInputLogRead(const P::Message& m)
{
    P::InputLogRead l;
    int id;
    Instance* inst;
    if (!Parse(m, l) || !Resolve(m, id, inst))
        return;
    size_t max = P::ResolveReadMax(l.Max);
    std::vector<InputLogEntry> log = inst->Input.ReadLog(l.FromFrame, max + 1);
    P::InputLogReply r;
    r.Seq = l.Seq;
    size_t bytes = 0;
    for (const InputLogEntry& e : log)
    {
        size_t size = 22 + e.Touch.size() * 12 + e.Mic.size() * 2;
        if (r.Entries.size() >= max || bytes + size > P::MaxReplyBytes)
        {
            r.Truncated = 1;
            break;
        }
        bytes += size;
        P::InputLogEntry out;
        out.Frame = e.Frame;
        out.Payload.Seq = e.Seq;
        out.Payload.Buttons = e.Buttons;
        out.Payload.Lid = e.Lid;
        out.Payload.MicKind = e.MicKind;
        out.Payload.MicLevel = e.MicLevel;
        for (const TouchPoint& t : e.Touch)
            out.Payload.Touch.push_back({t.Offset, t.X, t.Y, t.Down});
        out.Payload.Mic = e.Mic;
        r.Entries.push_back(std::move(out));
    }
    Reply(uint16_t(id), r);
}

void Server::DoSymbolsLoad(const P::Message& m)
{
    P::SymbolsLoad s;
    int id;
    Instance* inst;
    if (!Parse(m, s) || !Resolve(m, id, inst))
        return;
    P::SymbolsLoadReply r;
    r.Seq = s.Seq;
    if (s.Text.empty())
        inst->Symbols.clear();
    else
    {
        std::string error;
        if (!inst->Symbols.load(s.Text, error))
            return Error(uint16_t(id), s.Seq, P::ErrorCode::failed, error);
        if (auto set = inst->Symbols.set())
        {
            r.Symbols = uint32_t(set->SymbolCount);
            r.Modules = uint32_t(set->Modules.size());
        }
    }
    Reply(uint16_t(id), r);
}

void Server::DoPacing(const P::Message& m)
{
    P::Pacing p;
    if (!Parse(m, p))
        return;
    mds_set_pacing(int(std::min<uint32_t>(p.Value, 1000000)));
    Ok(m.Head.Console, p.Seq);
}

void Server::DoMpTimeout(const P::Message& m)
{
    P::MpTimeout p;
    if (!Parse(m, p))
        return;
    if (mds_mp_timeout(int(std::min<uint32_t>(p.Value, 3600000))) < 0)
        return FailFromApi(m.Head.Console, p.Seq);
    Ok(m.Head.Console, p.Seq);
}

void Server::Handle(const P::Message& m)
{
    if (m.Body.size() < 4)
    {
        Error(m.Head.Console, 0, P::ErrorCode::bad_payload, "message has no seq");
        return;
    }
    uint32_t seq = Le32(m.Body);
    switch (P::MsgType(m.Head.Type))
    {
    case P::MsgType::hello: DoHello(m); return;
    case P::MsgType::set_option: DoSetOption(m); return;
    case P::MsgType::console_create: DoConsoleCreate(m); return;
    case P::MsgType::console_destroy: DoConsoleDestroy(m); return;
    case P::MsgType::console_list: DoConsoleList(m); return;
    case P::MsgType::rom_load: DoRomLoad(m); return;
    case P::MsgType::reset: DoReset(m); return;
    case P::MsgType::shutdown: DoShutdown(m); return;
    case P::MsgType::run: DoRun(m); return;
    case P::MsgType::live_start: DoLiveStart(m); return;
    case P::MsgType::live_stop: DoLiveStop(m); return;
    case P::MsgType::pause: DoPause(m); return;
    case P::MsgType::step: DoStep(m); return;
    case P::MsgType::input: DoInput(m); return;
    case P::MsgType::input_multi: DoInputMulti(m); return;
    case P::MsgType::read: DoRead(m); return;
    case P::MsgType::write: DoWrite(m); return;
    case P::MsgType::registers: DoRegisters(m); return;
    case P::MsgType::set_register: DoSetRegister(m); return;
    case P::MsgType::screenshot: DoScreenshot(m); return;
    case P::MsgType::memory_search: DoMemorySearch(m); return;
    case P::MsgType::text_find: DoTextFind(m); return;
    case P::MsgType::subscribe: DoSubscribe(m); return;
    case P::MsgType::unsubscribe: DoUnsubscribe(m); return;
    case P::MsgType::input_log_read: DoInputLogRead(m); return;
    case P::MsgType::breakpoint_add: DoBreakpointAdd(m); return;
    case P::MsgType::watchpoint_add: DoWatchpointAdd(m); return;
    case P::MsgType::point_remove: DoPointRemove(m); return;
    case P::MsgType::point_list: DoPointList(m); return;
    case P::MsgType::point_enable: DoPointEnable(m); return;
    case P::MsgType::savestate_save: DoSavestateSave(m); return;
    case P::MsgType::savestate_load: DoSavestateLoad(m); return;
    case P::MsgType::lua_run: DoLuaRun(m); return;
    case P::MsgType::lua_exec: DoLuaExec(m); return;
    case P::MsgType::lua_stop: DoLuaStop(m); return;
    case P::MsgType::trace_config: DoTraceConfig(m); return;
    case P::MsgType::trace_read: DoTraceRead(m); return;
    case P::MsgType::calls_track: DoCallsTrack(m); return;
    case P::MsgType::callstack: DoCallStack(m); return;
    case P::MsgType::call_counts_read: DoCallCountsRead(m); return;
    case P::MsgType::coverage_track: DoCoverageTrack(m); return;
    case P::MsgType::coverage_read: DoCoverageRead(m); return;
    case P::MsgType::writers_track: DoWritersTrack(m); return;
    case P::MsgType::last_writer: DoLastWriter(m); return;
    case P::MsgType::packets_track: DoPacketsTrack(m); return;
    case P::MsgType::packets_read: DoPacketsRead(m); return;
    case P::MsgType::symbols_load: DoSymbolsLoad(m); return;
    case P::MsgType::pacing: DoPacing(m); return;
    case P::MsgType::mp_timeout: DoMpTimeout(m); return;
    default: break;
    }

    if (!P::IsKnownType(m.Head.Type) || m.Head.Type >= 0x8000)
        Error(m.Head.Console, seq, P::ErrorCode::unknown_type, "unknown message type");
    else
        Error(m.Head.Console, seq, P::ErrorCode::not_supported, "not supported yet");
}

// ----------------------------------------------------------------------------

}   // anonymous namespace

bool ServerStart(uint32_t address, int port, int range, int& bound, std::string& error)
{
    std::lock_guard lock(TheServerLock);
    if (!TheServer)
        TheServer = new Server();       // lives as long as the process: never joined during unload
    return TheServer->Start(address, port, range, bound, error);
}

void ServerStop()
{
    std::lock_guard lock(TheServerLock);
    if (TheServer)
        TheServer->Stop();
}

int ServerWait(int timeoutMs)
{
    Server* s;
    {
        std::lock_guard lock(TheServerLock);
        s = TheServer;
    }
    return s ? s->Wait(timeoutMs) : 0;
}

void ServerSetName(const std::string& name)
{
    std::lock_guard lock(TheServerLock);
    if (!TheServer)
        TheServer = new Server();
    TheServer->SetName(name);
}

}
