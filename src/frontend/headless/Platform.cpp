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

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "Platform.h"
#include "SPI_Firmware.h"
#include "MPInterface.h"
#include "Net.h"
#include "Instance.h"

namespace fs = std::filesystem;

namespace melonDS::Platform
{

static const auto StartTime = std::chrono::steady_clock::now();

static Headless::Instance* Inst(void* userdata)
{
    return static_cast<Headless::Instance*>(userdata);
}

static FILE* OpenNative(const std::string& path, const char* mode)
{
#ifdef _WIN32
    std::wstring wmode(mode, mode + strlen(mode));
    return _wfopen(fs::u8path(path).c_str(), wmode.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

void SignalStop(StopReason reason, void* userdata)
{
    if (userdata)
        Inst(userdata)->OnStop(reason);
}

static std::string ModeString(FileMode mode, bool exists)
{
    std::string m;
    if (mode & FileMode::Append)
        m += 'a';
    else if (!(mode & FileMode::Write))
        m += 'r';
    else if (mode & FileMode::NoCreate)
        m += 'r';
    else if ((mode & FileMode::Preserve) && exists)
        m += 'r';
    else
        m += 'w';
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite || ((mode & FileMode::Write) && m == "r"))
        m += '+';
    if (!(mode & FileMode::Text))
        m += 'b';
    return m;
}

std::string GetLocalFilePath(const std::string& filename)
{
    fs::path p = fs::u8path(filename);
    if (p.is_absolute())
        return filename;
    return (fs::u8path(Headless::LocalDirectory()) / p).u8string();
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;
    std::error_code ec;
    bool exists = fs::exists(fs::u8path(path), ec);
    if (ec || ((mode & FileMode::NoCreate) && !exists))
        return nullptr;
    return reinterpret_cast<FileHandle*>(OpenNative(path, ModeString(mode, exists).c_str()));
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    std::error_code ec;
    return fs::exists(fs::u8path(name), ec) && !ec;
}

bool LocalFileExists(const std::string& name)
{
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath)
{
    FILE* f = OpenNative(filepath, "ab");
    if (!f)
        return false;
    fclose(f);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath)
{
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file)
{
    return fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    return (u64)ftello64(reinterpret_cast<FILE*>(file));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int whence = SEEK_SET;
    if (origin == FileSeekOrigin::Current)
        whence = SEEK_CUR;
    else if (origin == FileSeekOrigin::End)
        whence = SEEK_END;
    return fseeko64(reinterpret_cast<FILE*>(file), offset, whence) == 0;
}

void FileRewind(FileHandle* file)
{
    rewind(reinterpret_cast<FILE*>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return fread(data, size, count, reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file)
{
    return fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return fwrite(data, size, count, reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return ret < 0 ? 0 : (u64)ret;
}

u64 FileLength(FileHandle* file)
{
    FILE* f = reinterpret_cast<FILE*>(file);
    s64 pos = ftello64(f);
    fseeko64(f, 0, SEEK_END);
    s64 len = ftello64(f);
    fseeko64(f, pos, SEEK_SET);
    return (u64)len;
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (level < Headless::LogThreshold())
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

Thread* Thread_Create(std::function<void()> func)
{
    return reinterpret_cast<Thread*>(new std::thread(std::move(func)));
}

void Thread_Free(Thread* thread)
{
    auto t = reinterpret_cast<std::thread*>(thread);
    if (t->joinable())
        t->detach();
    delete t;
}

void Thread_Wait(Thread* thread)
{
    auto t = reinterpret_cast<std::thread*>(thread);
    if (t->joinable())
        t->join();
}

struct SemaphoreImpl
{
    std::mutex Lock;
    std::condition_variable Cond;
    int Count = 0;
};

Semaphore* Semaphore_Create()
{
    return reinterpret_cast<Semaphore*>(new SemaphoreImpl);
}

void Semaphore_Free(Semaphore* sema)
{
    delete reinterpret_cast<SemaphoreImpl*>(sema);
}

void Semaphore_Reset(Semaphore* sema)
{
    auto s = reinterpret_cast<SemaphoreImpl*>(sema);
    std::lock_guard lock(s->Lock);
    s->Count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    auto s = reinterpret_cast<SemaphoreImpl*>(sema);
    std::unique_lock lock(s->Lock);
    s->Cond.wait(lock, [s] { return s->Count > 0; });
    s->Count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    auto s = reinterpret_cast<SemaphoreImpl*>(sema);
    std::unique_lock lock(s->Lock);
    if (!s->Cond.wait_for(lock, std::chrono::milliseconds(timeout_ms), [s] { return s->Count > 0; }))
        return false;
    s->Count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    auto s = reinterpret_cast<SemaphoreImpl*>(sema);
    {
        std::lock_guard lock(s->Lock);
        s->Count += count;
    }
    s->Cond.notify_all();
}

Mutex* Mutex_Create()
{
    return reinterpret_cast<Mutex*>(new std::mutex);
}

void Mutex_Free(Mutex* mutex)
{
    delete reinterpret_cast<std::mutex*>(mutex);
}

void Mutex_Lock(Mutex* mutex)
{
    reinterpret_cast<std::mutex*>(mutex)->lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    reinterpret_cast<std::mutex*>(mutex)->unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return reinterpret_cast<std::mutex*>(mutex)->try_lock();
}

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - StartTime).count();
}

u64 GetUSCount()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - StartTime).count();
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    if (userdata)
        Inst(userdata)->WriteSave(savedata, savelen);
}

void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata)
{
}

void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata)
{
}

void MP_Begin(void* userdata)
{
    MPInterface::Get().Begin(Inst(userdata)->ID);
}

void MP_End(void* userdata)
{
    MPInterface::Get().End(Inst(userdata)->ID);
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
    Inst(userdata)->LogPacket(true, 0, data, len, timestamp);
    return MPInterface::Get().SendPacket(Inst(userdata)->ID, data, len, timestamp);
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    int len = MPInterface::Get().RecvPacket(Inst(userdata)->ID, data, timestamp);
    if (len > 0)
        Inst(userdata)->LogPacket(false, 0, data, len, *timestamp);
    return len;
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
    Inst(userdata)->LogPacket(true, 1, data, len, timestamp);
    return MPInterface::Get().SendCmd(Inst(userdata)->ID, data, len, timestamp);
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
    Inst(userdata)->LogPacket(true, 2, data, len, timestamp);
    return MPInterface::Get().SendReply(Inst(userdata)->ID, data, len, timestamp, aid);
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
    Inst(userdata)->LogPacket(true, 3, data, len, timestamp);
    return MPInterface::Get().SendAck(Inst(userdata)->ID, data, len, timestamp);
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    int len = MPInterface::Get().RecvHostPacket(Inst(userdata)->ID, data, timestamp);
    if (len > 0)
        Inst(userdata)->LogPacket(false, 1, data, len, *timestamp);
    return len;
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    return MPInterface::Get().RecvReplies(Inst(userdata)->ID, data, timestamp, aidmask);
}

int Net_SendPacket(u8* data, int len, void* userdata)
{
    Headless::Network().SendPacket(data, len, Inst(userdata)->ID);
    return 0;
}

int Net_RecvPacket(u8* data, void* userdata)
{
    return Headless::Network().RecvPacket(data, Inst(userdata)->ID);
}

void Camera_Start(int num, void* userdata)
{
}

void Camera_Stop(int num, void* userdata)
{
}

void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata)
{
    memset(frame, 0, width * height * (yuv ? 2 : 4));
}

void Mic_Start(void* userdata)
{
}

void Mic_Stop(void* userdata)
{
}

int Mic_ReadInput(s16* data, int maxlength, void* userdata)
{
    memset(data, 0, maxlength * sizeof(s16));
    return maxlength;
}

AACDecoder* AAC_Init()
{
    return nullptr;
}

void AAC_DeInit(AACDecoder* dec)
{
}

bool AAC_Configure(AACDecoder* dec, int frequency, int channels)
{
    return false;
}

bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen)
{
    return false;
}

bool Addon_KeyDown(KeyType type, void* userdata)
{
    return false;
}

void Addon_RumbleStart(u32 len, void* userdata)
{
}

void Addon_RumbleStop(void* userdata)
{
}

float Addon_MotionQuery(MotionQueryType type, void* userdata)
{
    return 0;
}

DynamicLibrary* DynamicLibrary_Load(const char* lib)
{
#ifdef _WIN32
    return reinterpret_cast<DynamicLibrary*>(LoadLibraryA(lib));
#else
    return reinterpret_cast<DynamicLibrary*>(dlopen(lib, RTLD_NOW | RTLD_LOCAL));
#endif
}

void DynamicLibrary_Unload(DynamicLibrary* lib)
{
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

}
