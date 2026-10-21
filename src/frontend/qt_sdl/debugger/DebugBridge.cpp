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
#include <chrono>

#include <QThread>

#include "DebugBridge.h"
#include "DebugSymbolsQt.h"
#include "FrameTools.h"
#include "LogWindow.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "ARM.h"

using namespace melonDS;

DebugBridge::DebugBridge(EmuInstance* inst) : QObject(nullptr), Inst(inst), Symbols(std::make_unique<DebugSymbols>()),
    Frames(std::make_unique<FrameTools>(inst, this))
{
    qRegisterMetaType<melonDS::BreakEvent>("melonDS::BreakEvent");
    EmuThread* thread = inst->getEmuThread();
    connect(thread, &EmuThread::windowEmuPause, this, &DebugBridge::pauseChanged);
    connect(thread, &EmuThread::windowEmuStart, this, [this] {
        emit started();
        emit pauseChanged(false);
    });
    connect(thread, &EmuThread::windowEmuStop, this, [this] { emit pauseChanged(true); });
}

DebugBridge::~DebugBridge()
{
    for (QPointer<QWidget>& w : Windows)
        delete w.data();
}

void DebugBridge::adopt(QWidget* window)
{
    Windows.erase(std::remove_if(Windows.begin(), Windows.end(), [](const QPointer<QWidget>& w) { return w.isNull(); }), Windows.end());
    Windows.emplace_back(window);
}

bool DebugBridge::onEmuThread() const
{
    return QThread::currentThread() == Inst->getEmuThread();
}

void DebugBridge::post(Job job)
{
    {
        std::lock_guard lock(JobLock);
        Jobs.push_back(std::move(job));
    }
    JobCond.notify_all();
    std::lock_guard lock(ConsoleLock);
    if (Console && Console->IsDebugMode())
        Console->Debugger.RequestService();
}

namespace
{
struct SyncCall
{
    std::mutex Lock;
    std::condition_variable Cond;
    bool Cancelled = false;
    bool Started = false;
    bool Done = false;
};
}

bool DebugBridge::run(const Job& job, int timeoutMs)
{
    if (onEmuThread())
    {
        if (!Console)
            return false;
        job(*Console);
        return true;
    }

    auto sync = std::make_shared<SyncCall>();
    post([sync, &job](NDS& nds) {
        {
            std::lock_guard lock(sync->Lock);
            if (sync->Cancelled)
                return;
            sync->Started = true;
        }
        job(nds);
        std::lock_guard lock(sync->Lock);
        sync->Done = true;
        sync->Cond.notify_all();
    });

    std::unique_lock lock(sync->Lock);
    if (sync->Cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return sync->Done; }))
        return true;
    if (!sync->Started)
    {
        sync->Cancelled = true;
        return false;
    }
    sync->Cond.wait(lock, [&] { return sync->Done; });
    return true;
}

void DebugBridge::attachConsole(NDS* nds)
{
    {
        std::lock_guard lock(ConsoleLock);
        Console = nds;
    }
    if (!nds)
        return;
    nds->Debugger.SetBreakHandler([this](const BreakEvent& ev) { onBreak(ev); });
    nds->Debugger.SetServiceHandler([this] { runJobs(); });
    int instance = Inst->getInstanceID();
    nds->Debugger.PrintHandler = [instance](u32 cpu, const char* text, bool newline) {
        DebugLog::append(instance, cpu ? DebugLog::source_ARM7 : DebugLog::source_ARM9, text, newline);
    };
    for (const SavedPoint& p : Saved)
    {
        int id = p.Exec ? nds->Debugger.AddBreakpoint(p.CPUMask, p.Start)
                        : nds->Debugger.AddWatchpoint(p.CPUMask, p.Start, p.Size, p.Flags);
        if (id >= 0 && !p.Enabled)
            nds->Debugger.SetEnabled(id, false);
    }
    Saved.clear();
    if (CallUsers > 0)
        nds->Debugger.SetCallTracking(true);
    emit consoleChanged();
}

void DebugBridge::detachConsole()
{
    NDS* nds;
    {
        std::lock_guard lock(ConsoleLock);
        nds = Console;
        Console = nullptr;
    }
    if (!nds)
        return;
    clearTemps(*nds);
    Saved.clear();
    for (const Breakpoint& b : nds->Debugger.GetBreakpoints())
        if (!b.Callback)
            Saved.push_back({true, b.CPUMask, 0, b.Enabled, b.Addr, 1});
    for (const Watchpoint& w : nds->Debugger.GetWatchpoints())
        if (!w.Callback)
            Saved.push_back({false, w.CPUMask, w.Flags, w.Enabled, w.Start, w.End - w.Start + 1});
    nds->Debugger.SetBreakHandler(nullptr);
    nds->Debugger.SetServiceHandler(nullptr);
}

bool DebugBridge::hasConsole() const
{
    std::lock_guard lock(ConsoleLock);
    return Console != nullptr;
}

bool DebugBridge::debugMode() const
{
    std::lock_guard lock(ConsoleLock);
    return Console && Console->IsDebugMode();
}

bool DebugBridge::isRunning() const
{
    EmuThread* thread = Inst->getEmuThread();
    return !Broken && thread->emuIsActive() && thread->emuIsRunning();
}

BreakEvent DebugBridge::lastBreak() const
{
    std::lock_guard lock(BreakLock);
    return Last;
}

u32 DebugBridge::currentPC(NDS& nds, int cpu)
{
    ARM& arm = cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
    return arm.R[15] - ((arm.CPSR & 0x20) ? 2 : 4);
}

void DebugBridge::runJobs()
{
    std::deque<Job> jobs;
    {
        std::lock_guard lock(JobLock);
        jobs.swap(Jobs);
    }
    if (!Console)
        return;
    for (Job& job : jobs)
        job(*Console);
}

void DebugBridge::serviceFrame()
{
    Detaching = false;
    runJobs();
}

void DebugBridge::idle(int ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;)
    {
        runJobs();
        std::unique_lock lock(JobLock);
        if (Woken)
        {
            Woken = false;
            return;
        }
        if (!JobCond.wait_until(lock, deadline, [this] { return !Jobs.empty() || Woken; }))
            return;
    }
}

void DebugBridge::wake()
{
    {
        std::lock_guard lock(JobLock);
        Woken = true;
    }
    JobCond.notify_all();
}

void DebugBridge::onBreak(const BreakEvent& ev)
{
    if (Detaching || !Console)
        return;
    clearTemps(*Console);
    Console->Debugger.CancelBreakRequests();
    {
        std::lock_guard lock(BreakLock);
        Last = ev;
    }
    ResumeRequested = false;
    Broken = true;
    emit broke(ev);

    EmuThread* thread = Inst->getEmuThread();
    for (;;)
    {
        runJobs();
        if (ResumeRequested)
            break;
        int act = thread->handleParkedMessages();
        if (act == EmuThread::park_Resume)
            break;
        if (act == EmuThread::park_Detach)
        {
            Detaching = true;
            break;
        }
        std::unique_lock lock(JobLock);
        JobCond.wait_for(lock, std::chrono::milliseconds(50), [this] { return !Jobs.empty() || Woken; });
        Woken = false;
    }
    Broken = false;
    emit resumed();
}

void DebugBridge::clearTemps(NDS& nds)
{
    for (int id : Temps)
        nds.Debugger.Remove(id);
    Temps.clear();
}

void DebugBridge::unpauseEmu()
{
    Inst->getEmuThread()->debugResume();
}

void DebugBridge::addTemp(NDS& nds, int cpu, u32 addr, u32 minSP, u8 mode)
{
    ARM* arm = cpu ? (ARM*)nds.ARM7 : (ARM*)nds.ARM9;
    int id = nds.Debugger.AddBreakpoint(1 << cpu, addr, [arm, minSP, mode](const BreakEvent&) {
        if (!mode)
            return true;
        auto thread = [](u8 m) { return m == 0x10 ? 0x1F : m; };
        return thread(arm->CPSR & 0x1F) == thread(mode) && arm->R[13] >= minSP;
    });
    Temps.push_back(id);
}

bool DebugBridge::isTemp(int id) const
{
    return std::find(Temps.begin(), Temps.end(), id) != Temps.end();
}

void DebugBridge::requestBreak(int cpu)
{
    post([this, cpu](NDS& nds) {
        nds.Debugger.RequestBreak(1 << cpu);
        unpauseEmu();
    });
}

void DebugBridge::resume()
{
    post([this](NDS&) {
        ResumeRequested = true;
        unpauseEmu();
    });
}

void DebugBridge::step(int cpu, u32 count)
{
    post([this, cpu, count](NDS& nds) {
        nds.Debugger.Step(cpu, count);
        ResumeRequested = true;
        unpauseEmu();
    });
}

void DebugBridge::stepOver(int cpu)
{
    post([this, cpu](NDS& nds) {
        ARM& arm = cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
        u32 pc = currentPC(nds, cpu);
        u32 ret = nds.Debugger.CallReturn(cpu, pc, arm.CPSR & 0x20, arm.R[14]);
        if (ret)
            addTemp(nds, cpu, ret, arm.R[13], arm.CPSR & 0x1F);
        else
            nds.Debugger.Step(cpu, 1);
        ResumeRequested = true;
        unpauseEmu();
    });
}

void DebugBridge::stepTo(int cpu, StepMode mode)
{
    post([this, cpu, mode](NDS& nds) {
        nds.Debugger.StepTo(cpu, mode);
        ResumeRequested = true;
        unpauseEmu();
    });
}

void DebugBridge::stepOut(int cpu)
{
    post([this, cpu](NDS& nds) {
        ARM& arm = cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
        auto stack = nds.Debugger.GetCallStack(cpu);
        if (stack.empty())
            addTemp(nds, cpu, arm.R[14] & ~1u, arm.R[13], arm.CPSR & 0x1F);
        else if (stack.back().Exception)
            addTemp(nds, cpu, stack.back().Return, 0, 0);
        else
            addTemp(nds, cpu, stack.back().Return, stack.back().SP, stack.back().Mode);
        ResumeRequested = true;
        unpauseEmu();
    });
}

void DebugBridge::runTo(int cpu, u32 addr)
{
    post([this, cpu, addr](NDS& nds) {
        addTemp(nds, cpu, addr, 0, 0);
        ResumeRequested = true;
        unpauseEmu();
    });
}

void DebugBridge::retainCallTracking()
{
    if (CallUsers++ == 0)
        post([](NDS& nds) { nds.Debugger.SetCallTracking(true); });
}

void DebugBridge::releaseCallTracking()
{
    if (--CallUsers == 0)
        post([](NDS& nds) { nds.Debugger.SetCallTracking(false); });
}
