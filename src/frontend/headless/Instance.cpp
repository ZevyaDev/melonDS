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
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Instance.h"
#include "LuaHost.h"
#include "Args.h"
#include "GPU_Soft.h"
#include "Net_Slirp.h"
#include "Savestate.h"
#include "SPI_Firmware.h"

using namespace melonDS;
namespace fs = std::filesystem;

namespace Headless
{

static std::vector<u8> ReadFile(const std::string& path)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f)
        return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool WriteFile(const std::string& path, const void* data, size_t len)
{
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(static_cast<const char*>(data), len);
    return bool(f);
}

template <typename T>
static std::unique_ptr<T> LoadBIOS(const std::string& path, std::string& error)
{
    auto data = ReadFile(path);
    if (data.size() != std::tuple_size_v<T>)
    {
        error = "bad BIOS image: " + path;
        return nullptr;
    }
    auto bios = std::make_unique<T>();
    memcpy(bios->data(), data.data(), data.size());
    return bios;
}

static void CustomizeFirmware(Firmware& firmware, int effectiveId, const std::string& nickname)
{
    auto& user = firmware.GetEffectiveUserData();
    if (!nickname.empty())
    {
        size_t len = std::min<size_t>(nickname.size(), 10);
        for (size_t i = 0; i < len; i++)
            user.Nickname[i] = (char16_t)(u8)nickname[i];
        user.NameLength = len;
    }

    if (effectiveId > 0)
    {
        auto& header = firmware.GetHeader();
        MacAddress mac = header.MacAddr;
        mac[3] += effectiveId;
        mac[4] += effectiveId * 0x44;
        mac[5] += effectiveId * 0x10;
        mac[0] &= 0xFC;
        header.MacAddr = mac;
        header.UpdateChecksum();
    }
    firmware.UpdateChecksums();
}

static std::atomic<int> Base {0};

int InstanceBase()
{
    return Base;
}

bool SetInstanceBase(int base)
{
    if (base < 0)
        return false;
    Base = base;
    return true;
}

std::string DefaultSavePath(const std::string& romPath, int effectiveId)
{
    std::string save = fs::u8path(romPath).replace_extension(".sav").u8string();
    if (effectiveId > 0)
        save += "." + std::to_string(effectiveId + 1);
    return save;
}

int Instance::EffectiveID() const
{
    return ID + InstanceBase();
}

Instance::Instance(int id, const Config& config, std::string& error) : ID(id)
{
    NDSArgs args {};
    args.JIT = std::nullopt;
    args.Debug = config.Debug;

    if (!config.BIOS9Path.empty())
        if (auto bios = LoadBIOS<ARM9BIOSImage>(config.BIOS9Path, error))
            args.ARM9BIOS = std::move(bios);
    if (!config.BIOS7Path.empty())
        if (auto bios = LoadBIOS<ARM7BIOSImage>(config.BIOS7Path, error))
            args.ARM7BIOS = std::move(bios);
    if (!error.empty())
        return;

    if (!config.FirmwarePath.empty())
    {
        Platform::FileHandle* f = Platform::OpenFile(config.FirmwarePath, Platform::FileMode::Read);
        if (!f)
        {
            error = "cannot open firmware: " + config.FirmwarePath;
            return;
        }
        Firmware firmware(f);
        Platform::CloseFile(f);
        if (!firmware.Buffer())
        {
            error = "bad firmware image: " + config.FirmwarePath;
            return;
        }
        args.Firmware = std::move(firmware);
    }
    CustomizeFirmware(args.Firmware, EffectiveID(), config.Nickname);

    NDS = std::make_unique<melonDS::NDS>(std::move(args), this);
    NDS->SetRenderer(std::make_unique<SoftRenderer>(*NDS));
    NDS->Reset();
    NDS->SetInputTimeline(&TouchLine);
    NDS->SetSavestateExtra([this](Savestate* file) { return DoSavestateExtra(file); });
    NDS->Debugger.SetBreakHandler([this](const BreakEvent& ev) { Park(ev); });
    NDS->Debugger.Probes.FrameCounter = &FramesRun;   // stamps probe, memory log and action records
    NDS->Debugger.ResetHandler = [this](u8 cpu, bool console) { OnGdbReset(cpu, console); };
    Network().RegisterInstance(ID);
    Lua = std::make_unique<LuaHost>(*this);

    Commands.SetWake([this] {
        // Take the lock so a thread between its predicate check and its wait cannot miss this.
        { std::lock_guard lock(Lock); }
        Cond.notify_all();
    });
    Input.SetWake([this] {
        { std::lock_guard lock(Lock); }
        Cond.notify_all();
    });
    Input.SetBarrierOwner(ID);
    Barriers().Register(ID, [this] {
        { std::lock_guard lock(Lock); BarrierEvent = true; }
        Cond.notify_all();
    });
    Thread = std::thread([this] { ThreadMain(); });
    Commands.SetDrainThread(Thread.get_id());
}

Instance::~Instance()
{
    if (!NDS)       // the constructor failed before the console existed (bad BIOS or firmware)
        return;
    {
        std::lock_guard lock(Lock);
        Quit = true;
        FramesLeft = 0;
    }
    Commands.Shutdown();
    NDS->Debugger.SetBreakHandler(nullptr);
    NDS->Debugger.ResetHandler = nullptr;
    Cond.notify_all();
    if (Thread.joinable())
        Thread.join();
    Barriers().Unregister(ID);
    Barriers().DropConsole(ID);     // partners waiting for this console's payload are released
    Lua.reset();
    Network().UnregisterInstance(ID);
    NDS->Stop();
}

bool Instance::LoadROM(const std::string& path, const std::string& savePath, std::string& error)
{
    auto rom = ReadFile(path);
    if (rom.empty())
    {
        error = "cannot read ROM: " + path;
        return false;
    }

    SavePath = savePath.empty() ? DefaultSavePath(path, EffectiveID()) : savePath;

    std::optional<NDSCart::NDSCartArgs> cartArgs;
    auto save = ReadFile(SavePath);
    if (!save.empty())
    {
        NDSCart::NDSCartArgs ca;
        ca.SRAM = std::make_unique<u8[]>(save.size());
        memcpy(ca.SRAM.get(), save.data(), save.size());
        ca.SRAMLength = save.size();
        cartArgs = std::move(ca);
    }

    auto cart = NDSCart::ParseROM(rom.data(), rom.size(), this, std::move(cartArgs));
    if (!cart)
    {
        error = "cannot parse ROM: " + path;
        return false;
    }

    ROMName = fs::u8path(path).filename().u8string();
    NDS->SetNDSCart(std::move(cart));
    ClearCheats();      // codes are written for one game
    Reset();
    return true;
}

void Instance::Reset()
{
    NDS->Reset();
    if (NDS->CartInserted() && NDS->NeedsDirectBoot())
        NDS->SetupDirectBoot(ROMName);
    NDS->Start();
    FramesRun = 0;
    EmuFrame = 0;
    Resets++;
    Input.Reset();
    TouchLine.Reset();
    Mic.Clear();
    Lua->OnReset();
}

u32 NextInstruction(const ARM& cpu)
{
    return cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
}

static std::atomic<int> PacingFPS = 0;

void SetPacing(int fps)
{
    PacingFPS = fps > 0 ? fps : 0;
}

void Instance::Start(u32 frames)
{
    {
        std::lock_guard lock(Lock);
        if (State == RunState::Running)
            return;
        FramesLeft = frames;
        DropParkEvents();
        State = RunState::Running;
    }
    Cond.notify_all();
}

void Instance::StopAfter(u32 frames)
{
    {
        std::lock_guard lock(Lock);
        if (State == RunState::Running)
            FramesLeft = frames;
    }
    Cond.notify_all();      // a console waiting for input has to see FramesLeft change
}

RunState Instance::Wait(int timeoutMs)
{
    std::unique_lock lock(Lock);
    auto done = [this] { return State != RunState::Running; };
    if (timeoutMs < 0)
        Cond.wait(lock, done);
    else
        Cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), done);
    return State;
}

RunState Instance::GetState()
{
    std::lock_guard lock(Lock);
    return State;
}

// Only a running console is interrupted: a request left armed on a stopped one would park its next run at its
// first instruction. The request is made under Lock, so it cannot slip in after the run has ended (StopRunning)
// or the console has parked (Park), both of which drop the requests left over.
void Instance::Interrupt()
{
    std::lock_guard lock(Lock);
    if (State != RunState::Running)
        return;
    if (Waiting)
    {
        // No CPU is running to take a break request: park at the frame boundary instead.
        WaitInterrupt = true;
        Cond.notify_all();
        return;
    }
    NDS->Debugger.RequestBreak(3);
}

// The run is over (Lock held, emulator thread): an interrupt that arrived too late to park it must not park the
// next run. The step countdowns stay armed (mds_step before mds_run).
void Instance::StopRunning()
{
    State = RunState::Idle;
    WaitInterrupt = false;
    NDS->Debugger.CancelBreakRequests(false);
}

bool Instance::WaitingForInput()
{
    std::lock_guard lock(Lock);
    return Waiting;
}

bool Instance::TakeBreak(BreakEvent& out)
{
    std::lock_guard lock(Lock);
    if (Breaks.empty())
        return false;
    out = Breaks.front();
    Breaks.pop_front();
    return true;
}

void Instance::DropParkEvents()
{
    Breaks.erase(std::remove_if(Breaks.begin(), Breaks.end(),
                                [](const BreakEvent& e) { return e.Kind != BreakKind::Reset; }),
                 Breaks.end());
}

// A GDB client reset the console (vRun, R, r) or one CPU ("monitor reset") on the emulator thread, inside a frame,
// while the console runs. Queue a reset event for mds_take_break (the socket server forwards it as a break event)
// and count it. A whole-console reset restarts the emulated frame count, as mds_reset does; the input queue, the
// process frame count (mds_frames) and the cheat list are kept.
void Instance::OnGdbReset(u8 cpu, bool console)
{
    BreakEvent ev {};
    ev.Kind = BreakKind::Reset;
    ev.CPU = cpu & 1;
    ev.PC = NextInstruction(cpu & 1 ? (ARM&)*NDS->ARM7 : (ARM&)*NDS->ARM9);
    ev.Value = console ? 1 : 0;
    if (console)
    {
        EmuFrame = 0;
        Lua->OnReset();
    }
    {
        std::lock_guard lock(Lock);
        auto isReset = [](const BreakEvent& e) { return e.Kind == BreakKind::Reset; };
        if (std::count_if(Breaks.begin(), Breaks.end(), isReset) >= 64)     // keep the newest 64
            Breaks.erase(std::find_if(Breaks.begin(), Breaks.end(), isReset));
        Breaks.push_back(ev);
    }
    Resets++;
    Cond.notify_all();
}

SoftRenderer& Instance::Renderer()
{
    return static_cast<SoftRenderer&>(NDS->GPU.GetRenderer());
}

void Instance::ApplyRender()
{
    SoftRenderer& rend = Renderer();
    u32 skip = WantedRenderSkip.load();
    if (rend.GetRenderSkip() != skip)
        rend.SetRenderSkip(skip);
    ApplyThreaded3D();
}

void Instance::ApplyThreaded3D()
{
    SoftRenderer& rend = Renderer();
    bool threaded = WantedThreaded3D.load();
    if (rend.IsThreaded3D() != threaded)
        rend.SetThreaded3D(threaded);
}

bool Instance::Call(const std::function<void(Instance&)>& fn)
{
    return Commands.Call([this, &fn] { fn(*this); });
}

void Instance::ThreadMain()
{
    for (;;)
    {
        {
            std::unique_lock lock(Lock);
            Cond.wait(lock, [this] { return Quit || State == RunState::Running || Commands.HasPending(); });
            if (Quit)
                return;
            if (State != RunState::Running)
            {
                // woken by a command while idle
                lock.unlock();
                Commands.Drain();
                continue;
            }
        }
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            Commands.Drain();   // frame boundary: the only place the NDS is touched from outside
            {
                std::lock_guard lock(Lock);
                if (Quit || FramesLeft == 0)
                {
                    StopRunning();
                    break;
                }
            }
            AppliedInput applied;
            if (!AcquireInput(applied))     // may wait at a barrier or for input
            {
                std::lock_guard lock(Lock);
                if (Quit || FramesLeft == 0)
                {
                    StopRunning();
                    break;
                }
                continue;
            }
            ApplyInput(*this, applied);
            ApplyRender();
            NDS->RunFrame();
            FramesRun++;
            EmuFrame++;
            Lua->OnFrameEnd();
            Subs.Publish(*this, applied);
            {
                std::lock_guard lock(Lock);
                if (FramesLeft)
                    FramesLeft--;
            }
            if (int fps = PacingFPS)
            {
                next += std::chrono::microseconds(1000000 / fps);
                auto now = std::chrono::steady_clock::now();
                if (next > now)
                    std::this_thread::sleep_until(next);
                else
                    next = now;
            }
        }
        // The console stopped running: a barrier partner must not wait for a payload it will not reach.
        Barriers().DropConsole(ID);
        Cond.notify_all();
    }
}

// Wait at the frame boundary until every console of the barrier group has reached its payload (or can
// no longer reach it, or a partner is blocked in a wireless receive). True: go ahead and pop the payload.
// False: the caller should look at Quit/FramesLeft again before running a frame. Like the wait for input,
// the wait ends for Quit, a stop, an interrupt (which parks) and posted commands (which are drained).
bool Instance::AwaitBarrier(uint32_t group)
{
    BarrierGroups& groups = Barriers();
    groups.Arrive(group, ID);

    std::unique_lock lock(Lock);
    bool waited = false;
    for (;;)
    {
        // Clear the event before looking: one that arrives after the look sets it again.
        BarrierEvent = false;
        lock.unlock();
        bool released = groups.Released(group, ID);
        lock.lock();
        if (released)
        {
            Waiting = false;
            return true;
        }

        Waiting = true;
        if (!waited)
            Cond.notify_all();
        waited = true;
        Cond.wait(lock, [this, group] {
            return Quit || FramesLeft == 0 || WaitInterrupt || Commands.HasPending() || BarrierEvent
                || Input.FrontBarrier() != group;
        });
        if (Quit || FramesLeft == 0)
        {
            Waiting = false;
            return false;
        }
        if (WaitInterrupt)
        {
            Waiting = false;
            WaitInterrupt = false;
            BreakEvent ev {};
            ev.Kind = BreakKind::Request;
            ev.PC = NextInstruction(*NDS->ARM9);
            lock.unlock();
            Park(ev);
            return false;
        }
        if (Commands.HasPending())
        {
            Waiting = false;
            lock.unlock();
            Commands.Drain();
            return false;
        }
        if (Input.FrontBarrier() != group)      // the queue was cleared under us
        {
            Waiting = false;
            return true;
        }
    }
}

// Resolve the input for the next frame. In Wait mode with nothing queued this blocks at the frame
// boundary (FramesLeft is not used up) until input arrives or something else needs attention.
// A payload of a barrier group is not popped until its group is released.
// False means the caller should look at Quit/FramesLeft again before running a frame.
bool Instance::AcquireInput(AppliedInput& out)
{
    for (;;)
    {
        if (uint32_t group = Input.FrontBarrier())
        {
            if (!AwaitBarrier(group))
                return false;
            if (Input.FrontBarrier() != group)
                continue;
        }
        if (Input.Next(FramesRun, out))
        {
            if (out.Barrier)
                Barriers().Applied(out.Barrier, ID);
            return true;
        }

        std::unique_lock lock(Lock);
        Waiting = true;
        Cond.notify_all();
        Cond.wait(lock, [this] {
            return Quit || FramesLeft == 0 || WaitInterrupt || Commands.HasPending()
                || Input.Depth() != 0 || Input.Mode() != EmptyMode::Wait;
        });
        Waiting = false;
        if (Quit || FramesLeft == 0)
            return false;
        if (WaitInterrupt)
        {
            WaitInterrupt = false;
            BreakEvent ev {};
            ev.Kind = BreakKind::Request;
            ev.PC = NextInstruction(*NDS->ARM9);
            lock.unlock();
            Park(ev);
            return false;
        }
        if (Commands.HasPending())
        {
            lock.unlock();
            Commands.Drain();
            return false;
        }
    }
}

void Instance::Park(const BreakEvent& ev)
{
    std::unique_lock lock(Lock);
    if (Quit)
        return;
    DropParkEvents();       // only the latest park is reported; reset events before it stay
    Breaks.push_back(ev);
    ParkedWith = ev;
    ParkedR15 = {NDS->ARM9->R[15], NDS->ARM7->R[15]};
    State = RunState::Parked;
    // mds_interrupt asks both CPUs to break and only the first one parks: the other CPU's request (and those of
    // a group's other consoles, which mds_wait interrupts) would park the resume again at once.
    NDS->Debugger.CancelBreakRequests(false);
    Cond.notify_all();
    while (!Quit && State != RunState::Running)
    {
        if (Commands.HasPending())
        {
            lock.unlock();
            Commands.Drain();
            lock.lock();
            continue;
        }
        Cond.wait(lock, [this] { return Quit || State == RunState::Running || Commands.HasPending(); });
    }
}

bool Instance::SaveState(const std::string& path, std::string& error)
{
    if (GetState() != RunState::Idle)
    {
        error = "savestates need a frame boundary; finish the frame first";
        return false;
    }
    Savestate state;
    if (state.Error || !NDS->DoSavestate(&state) || state.Error)
    {
        error = "savestate failed";
        return false;
    }
    if (!WriteFile(path, state.Buffer(), state.Length()))
    {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

bool Instance::LoadState(const std::string& path, std::string& error)
{
    if (GetState() != RunState::Idle)
    {
        error = "savestates need a frame boundary; finish the frame first";
        return false;
    }
    auto data = ReadFile(path);
    if (data.empty())
    {
        error = "cannot read " + path;
        return false;
    }
    Input.Clear();
    Mic.Clear();    // the load itself calls Mic_Start/Mic_Stop to match the saved state
    Savestate backup;
    NDS->DoSavestate(&backup);
    Savestate state(data.data(), data.size(), false);
    LoadedHeadlessSection = false;
    if (state.Error || !NDS->DoSavestate(&state) || state.Error)
    {
        Savestate restore(backup.Buffer(), backup.Length(), false);
        NDS->DoSavestate(&restore);
        error = "savestate rejected: " + path;
        return false;
    }
    if (!LoadedHeadlessSection && state.IsAtLeastVersion(14, 2))
    {
        // A 14.2 state from another frontend: the held input is what the console's registers say.
        u32 keys = NDS->KeyInput;
        InputQueue::Held held;
        held.Last.Buttons = (~keys & 0x3FF) | (((~keys >> 16) & 3) << 10);
        held.Last.Lid = (keys >> 23) & 1;
        held.Last.TouchDown = !(keys & (1 << 22));
        if (held.Last.TouchDown)
            NDS->SPI.GetTSC()->GetTouchCoords(held.Last.TouchX, held.Last.TouchY);
        Input.SetHeld(held);
        TouchLine.Restore(held.Last.TouchDown, held.Last.TouchX, held.Last.TouchY);
    }
    // Older states carry no input: the held input and EmuFrame stay as they were before the load.
    // Cheats are not part of a state: the loaded state is what a run from it starts with, so the codes added
    // before the load are dropped and a client re-adds the ones it wants (docs/headless-api.md, section 6).
    ClearCheats();
    return true;
}

void Instance::SyncCheats()
{
    std::vector<ARCode> codes;
    codes.reserve(Cheats.size());
    for (const CheatEntry& c : Cheats)
        codes.push_back(ARCode {nullptr, c.Name, "", c.Enabled, c.Code});
    NDS->AREngine.Cheats = std::move(codes);
}

int Instance::AddCheat(const std::string& name, std::vector<u32> code, bool enabled)
{
    int id = NextCheatID++;
    Cheats.push_back(CheatEntry {id, name, enabled, std::move(code)});
    SyncCheats();
    return id;
}

bool Instance::RemoveCheat(int id)
{
    for (auto it = Cheats.begin(); it != Cheats.end(); ++it)
        if (it->ID == id)
        {
            Cheats.erase(it);
            SyncCheats();
            return true;
        }
    return false;
}

bool Instance::EnableCheat(int id, bool enabled)
{
    for (CheatEntry& c : Cheats)
        if (c.ID == id)
        {
            c.Enabled = enabled;
            SyncCheats();
            return true;
        }
    return false;
}

int Instance::ClearCheats()
{
    int n = (int)Cheats.size();
    Cheats.clear();
    SyncCheats();
    return n;
}

// The headless frontend's own section ("HDLS", after the core's): the input the empty-queue rule holds, the
// pen state the touch timeline ended the frame in, the emulated frame count and the mic samples queued for
// the core. Version 1. States without it (older ones, or from another frontend) load without touching these.
bool Instance::DoSavestateExtra(Savestate* file)
{
    constexpr u32 Version = 1;
    if (!file->Saving && !file->HasSection("HDLS"))
        return true;
    file->Section("HDLS");
    u32 version = Version;
    file->Var32(&version);

    InputQueue::Held held = Input.GetHeld();
    bool down = false;
    u16 x = 0, y = 0;
    TouchLine.FinalState(down, x, y);
    u8 source = (u8)held.LastSource;
    u32 empty = (u32)held.EmptyFrames;
    u8 penDown = down;
    std::vector<int16_t> mic = Mic.Snapshot();
    u32 micCount = mic.size();
    u64 frame = EmuFrame;

    file->Var16(&held.Last.Buttons);
    file->Var8(&held.Last.Lid);
    file->Var8(&held.Last.TouchDown);
    file->Var16(&held.Last.TouchX);
    file->Var16(&held.Last.TouchY);
    file->Var8(&source);
    file->Var32(&empty);
    file->Var8(&penDown);
    file->Var16(&x);
    file->Var16(&y);
    file->Var64(&frame);
    file->Var32(&micCount);
    if (!file->Saving)
    {
        if (micCount > MicFifo::Capacity)
            return false;
        mic.resize(micCount);
    }
    if (micCount)
        file->VarArray(mic.data(), micCount * sizeof(int16_t));
    if (file->Error)
        return false;

    if (!file->Saving)
    {
        held.LastSource = source == (u8)InputSource::Client ? InputSource::Client : InputSource::Local;
        held.EmptyFrames = (int)empty;
        Input.SetHeld(held);
        TouchLine.Restore(penDown != 0, x, y);
        EmuFrame = frame;
        Mic.Restore(mic);
        LoadedHeadlessSection = true;
    }
    return true;
}

void Instance::SetPacketLog(bool enabled, size_t capacity)
{
    std::lock_guard lock(PacketLock);
    PacketLogging = enabled;
    PacketCapacity = capacity;
    Packets.clear();
}

std::vector<PacketRecord> Instance::DrainPackets()
{
    std::lock_guard lock(PacketLock);
    std::vector<PacketRecord> out(std::make_move_iterator(Packets.begin()), std::make_move_iterator(Packets.end()));
    Packets.clear();
    return out;
}

void Instance::LogPacket(bool sent, u8 type, const u8* data, int len, u64 timestamp)
{
    std::lock_guard lock(PacketLock);
    if (!PacketLogging || len <= 0)
        return;
    if (Packets.size() >= PacketCapacity)
        Packets.pop_front();
    Packets.push_back({timestamp, sent, type, std::vector<u8>(data, data + len)});
}

void Instance::OnStop(Platform::StopReason reason)
{
}

void Instance::WriteSave(const u8* data, u32 len)
{
    if (!SavePath.empty())
        WriteFile(SavePath, data, len);
}

static Net& NetInstance = *new Net();      // never destroyed, as the console registry: see Api.cpp
static std::once_flag NetInit;

Net& Network()
{
    std::call_once(NetInit, [] {
        NetInstance.SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            NetInstance.RXEnqueue(data, len);
        }));
    });
    return NetInstance;
}

static std::string LocalDir = ".";
static Platform::LogLevel Threshold = Platform::LogLevel::Warn;

const std::string& LocalDirectory()
{
    return LocalDir;
}

void SetLocalDirectory(const std::string& dir)
{
    LocalDir = dir;
}

Platform::LogLevel LogThreshold()
{
    return Threshold;
}

void SetLogThreshold(Platform::LogLevel level)
{
    Threshold = level;
}

}
