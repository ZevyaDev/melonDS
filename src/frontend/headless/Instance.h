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

#ifndef HEADLESS_INSTANCE_H
#define HEADLESS_INSTANCE_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Commands.h"
#include "DebugSymbols.h"
#include "Input.h"
#include "MicFifo.h"
#include "NDS.h"
#include "Net.h"
#include "Subs.h"

namespace Headless
{

struct Config
{
    std::string BIOS9Path;
    std::string BIOS7Path;
    std::string FirmwarePath;
    std::string Nickname;
    bool Debug = true;
};

// An Action Replay code in a console's cheat list (mds_cheat_*).
struct CheatEntry
{
    int ID;
    std::string Name;
    bool Enabled;
    std::vector<melonDS::u32> Code;     // the words the core's engine runs (ParseCheatCode)
};

struct PacketRecord
{
    melonDS::u64 Timestamp;
    bool Sent;
    melonDS::u8 Type;
    std::vector<melonDS::u8> Data;
};

enum class RunState
{
    Idle,
    Running,
    Parked,
};

class LuaHost;

class Instance
{
public:
    Instance(int id, const Config& config, std::string& error);
    ~Instance();

    const int ID;
    // The id the console presents to the outside: its slot plus the process-wide instance base.
    // Drives the firmware MAC address, the save-file suffix and emu.getconsoleid().
    int EffectiveID() const;
    std::unique_ptr<melonDS::NDS> NDS;
    std::unique_ptr<LuaHost> Lua;

    const std::string& GetSavePath() const { return SavePath; }
    bool LoadROM(const std::string& path, const std::string& savePath, std::string& error);
    void Reset();

    void Start(melonDS::u32 frames);
    void StopAfter(melonDS::u32 frames);
    RunState Wait(int timeoutMs);
    RunState GetState();
    void Interrupt();
    bool TakeBreak(melonDS::BreakEvent& out);

    // Run fn on this console's emulator thread (inline if already there) and wait.
    // False if the console is being destroyed and fn did not run.
    bool Call(const std::function<void(Instance&)>& fn);

    bool SaveState(const std::string& path, std::string& error);
    bool LoadState(const std::string& path, std::string& error);

    // The console's Action Replay codes, run by the core's engine (NDS::AREngine) every frame, in the order
    // added. Emulator thread only (Call). A savestate load and a ROM load clear the list; a reset keeps it.
    int AddCheat(const std::string& name, std::vector<melonDS::u32> code, bool enabled);
    bool RemoveCheat(int id);
    bool EnableCheat(int id, bool enabled);
    int ClearCheats();
    const std::vector<CheatEntry>& GetCheats() const { return Cheats; }

    void SetPacketLog(bool enabled, size_t capacity);
    std::vector<PacketRecord> DrainPackets();

    void OnStop(melonDS::Platform::StopReason reason);
    void WriteSave(const melonDS::u8* data, melonDS::u32 len);
    void LogPacket(bool sent, melonDS::u8 type, const melonDS::u8* data, int len, melonDS::u64 timestamp);

    // The event of the current (or last) park and each CPU's R15 when it parked. Emulator thread.
    const melonDS::BreakEvent& ParkEvent() const { return ParkedWith; }
    melonDS::u32 ParkR15(int cpu) const { return ParkedR15[cpu & 1]; }

    // True while the emulator thread is blocked at a frame boundary in Wait mode.
    bool WaitingForInput();

    // Resets of the console: mds_reset, mds_load_rom, and GDB restarts (whole console or one CPU).
    std::atomic<melonDS::u32> Resets {0};
    void OnGdbReset(melonDS::u8 cpu, bool console);

    melonDS::u64 FramesRun = 0;
    // Frames since the last reset as the emulated console sees them: savestates carry it (FramesRun stays the
    // process's count). Seeds the level-mic noise, so a frame gets the same noise with or without a load.
    melonDS::u64 EmuFrame = 0;
    // False for a console created with debug = 0: the debugger hooks never run, so the debugger calls fail.
    bool DebugEnabled() const { return NDS->IsDebugMode(); }
    InputQueue Input;
    Subscriptions Subs;     // frame events for the socket server's subscriptions
    TouchTimeline TouchLine;
    MicFifo Mic;
    DebugSymbols Symbols;   // this console's symbol table; the server thread only

private:
    std::string SavePath;
    std::string ROMName;
    std::thread Thread;
    std::mutex Lock;
    std::condition_variable Cond;
    RunState State = RunState::Idle;
    melonDS::u32 FramesLeft = 0;
    bool Quit = false;
    bool Waiting = false;           // blocked for input (guarded by Lock)
    bool WaitInterrupt = false;     // Interrupt() arrived while Waiting
    bool BarrierEvent = false;      // a barrier partner arrived, dropped out or entered a wireless receive
    // Undelivered events for mds_take_break, oldest first: at most one park (the latest) and GDB reset events.
    std::deque<melonDS::BreakEvent> Breaks;
    void DropParkEvents();
    melonDS::BreakEvent ParkedWith {};
    std::array<melonDS::u32, 2> ParkedR15 {};

    std::mutex PacketLock;
    bool PacketLogging = false;
    size_t PacketCapacity = 0;
    std::deque<PacketRecord> Packets;

    CommandQueue Commands;

    bool LoadedHeadlessSection = false;

    std::vector<CheatEntry> Cheats;
    int NextCheatID = 1;
    void SyncCheats();

    bool DoSavestateExtra(melonDS::Savestate* file);
    void ThreadMain();
    void Park(const melonDS::BreakEvent& ev);
    void StopRunning();
    bool AcquireInput(AppliedInput& out);
    bool AwaitBarrier(uint32_t group);
};

// Offset added to every console's id for MAC and save numbering. Process-wide; it can only
// change while no console exists. SetInstanceBase returns false for a negative base.
int InstanceBase();
bool SetInstanceBase(int base);
// "<rom>.sav" for effective id 0, "<rom>.sav.N+1" otherwise.
std::string DefaultSavePath(const std::string& romPath, int effectiveId);

// The address of the instruction the CPU executes next, between two instructions (idle, and at every park but a
// fault, which parks inside the faulting instruction).
melonDS::u32 NextInstruction(const melonDS::ARM& cpu);

melonDS::Net& Network();
const std::string& LocalDirectory();
void SetLocalDirectory(const std::string& dir);
melonDS::Platform::LogLevel LogThreshold();
void SetLogThreshold(melonDS::Platform::LogLevel level);
void SetPacing(int fps);

}

#endif
