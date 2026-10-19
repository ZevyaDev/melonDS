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

static void CustomizeFirmware(Firmware& firmware, int id, const std::string& nickname)
{
    auto& user = firmware.GetEffectiveUserData();
    if (!nickname.empty())
    {
        size_t len = std::min<size_t>(nickname.size(), 10);
        for (size_t i = 0; i < len; i++)
            user.Nickname[i] = (char16_t)(u8)nickname[i];
        user.NameLength = len;
    }

    if (id > 0)
    {
        auto& header = firmware.GetHeader();
        MacAddress mac = header.MacAddr;
        mac[3] += id;
        mac[4] += id * 0x44;
        mac[5] += id * 0x10;
        mac[0] &= 0xFC;
        header.MacAddr = mac;
        header.UpdateChecksum();
    }
    firmware.UpdateChecksums();
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
    CustomizeFirmware(args.Firmware, id, config.Nickname);

    NDS = std::make_unique<melonDS::NDS>(std::move(args), this);
    NDS->SetRenderer(std::make_unique<SoftRenderer>(*NDS));
    NDS->Reset();
    NDS->Debugger.SetBreakHandler([this](const BreakEvent& ev) { Park(ev); });
    Network().RegisterInstance(ID);
    Lua = std::make_unique<LuaHost>(*this);

    Thread = std::thread([this] { ThreadMain(); });
}

Instance::~Instance()
{
    {
        std::lock_guard lock(Lock);
        Quit = true;
        FramesLeft = 0;
    }
    NDS->Debugger.SetBreakHandler(nullptr);
    Cond.notify_all();
    if (Thread.joinable())
        Thread.join();
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

    SavePath = savePath.empty() ? (fs::u8path(path).replace_extension(".sav").u8string()) : savePath;
    if (ID > 0 && savePath.empty())
        SavePath += "." + std::to_string(ID + 1);

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
    Reset();
    return true;
}

void Instance::Reset()
{
    NDS->Reset();
    if (NDS->NeedsDirectBoot())
        NDS->SetupDirectBoot(ROMName);
    NDS->Start();
    FramesRun = 0;
    Lua->OnReset();
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
        BreakPending = false;
        State = RunState::Running;
    }
    Cond.notify_all();
}

void Instance::StopAfter(u32 frames)
{
    std::lock_guard lock(Lock);
    if (State == RunState::Running)
        FramesLeft = frames;
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

void Instance::Interrupt()
{
    NDS->Debugger.RequestBreak(3);
}

bool Instance::TakeBreak(BreakEvent& out)
{
    std::lock_guard lock(Lock);
    if (!BreakPending)
        return false;
    out = LastBreak;
    BreakPending = false;
    return true;
}

void Instance::ThreadMain()
{
    for (;;)
    {
        {
            std::unique_lock lock(Lock);
            Cond.wait(lock, [this] { return Quit || State == RunState::Running; });
            if (Quit)
                return;
        }
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            {
                std::lock_guard lock(Lock);
                if (Quit || FramesLeft == 0)
                {
                    State = RunState::Idle;
                    break;
                }
            }
            NDS->RunFrame();
            FramesRun++;
            Lua->OnFrameEnd();
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
        Cond.notify_all();
    }
}

void Instance::Park(const BreakEvent& ev)
{
    std::unique_lock lock(Lock);
    if (Quit)
        return;
    LastBreak = ev;
    BreakPending = true;
    State = RunState::Parked;
    Cond.notify_all();
    Cond.wait(lock, [this] { return Quit || State == RunState::Running; });
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
    Savestate backup;
    NDS->DoSavestate(&backup);
    Savestate state(data.data(), data.size(), false);
    if (state.Error || !NDS->DoSavestate(&state) || state.Error)
    {
        Savestate restore(backup.Buffer(), backup.Length(), false);
        NDS->DoSavestate(&restore);
        error = "savestate rejected: " + path;
        return false;
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

static Net NetInstance;
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
