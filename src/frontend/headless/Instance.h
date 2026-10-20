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

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Commands.h"
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
    std::unique_ptr<melonDS::NDS> NDS;
    std::unique_ptr<LuaHost> Lua;

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

    void SetPacketLog(bool enabled, size_t capacity);
    std::vector<PacketRecord> DrainPackets();

    void OnStop(melonDS::Platform::StopReason reason);
    void WriteSave(const melonDS::u8* data, melonDS::u32 len);
    void LogPacket(bool sent, melonDS::u8 type, const melonDS::u8* data, int len, melonDS::u64 timestamp);

    // True while the emulator thread is blocked at a frame boundary in Wait mode.
    bool WaitingForInput();

    melonDS::u64 FramesRun = 0;
    InputQueue Input;
    Subscriptions Subs;     // frame events for the socket server's subscriptions
    TouchTimeline TouchLine;
    MicFifo Mic;

private:
    std::string SavePath;
    std::string ROMName;
    std::thread Thread;
    std::mutex Lock;
    std::condition_variable Cond;
    RunState State = RunState::Idle;
    melonDS::u32 FramesLeft = 0;
    bool Quit = false;
    bool BreakPending = false;
    bool Waiting = false;           // blocked for input (guarded by Lock)
    bool WaitInterrupt = false;     // Interrupt() arrived while Waiting
    melonDS::BreakEvent LastBreak {};

    std::mutex PacketLock;
    bool PacketLogging = false;
    size_t PacketCapacity = 0;
    std::deque<PacketRecord> Packets;

    CommandQueue Commands;

    void ThreadMain();
    void Park(const melonDS::BreakEvent& ev);
    bool AcquireInput(AppliedInput& out);
};

melonDS::Net& Network();
const std::string& LocalDirectory();
void SetLocalDirectory(const std::string& dir);
melonDS::Platform::LogLevel LogThreshold();
void SetLogThreshold(melonDS::Platform::LogLevel level);
void SetPacing(int fps);

}

#endif
