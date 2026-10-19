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

#ifndef DEBUGBRIDGE_H
#define DEBUGBRIDGE_H

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include "NDS.h"

class EmuInstance;
class DebugSymbols;

class DebugBridge : public QObject
{
    Q_OBJECT

public:
    using Job = std::function<void(melonDS::NDS&)>;

    explicit DebugBridge(EmuInstance* inst);
    ~DebugBridge() override;

    EmuInstance* instance() const { return Inst; }
    DebugSymbols& symbols() { return *Symbols; }

    void post(Job job);
    bool run(const Job& job, int timeoutMs = 2000);

    template <typename Work, typename Done>
    void call(QObject* ctx, Work work, Done done)
    {
        using Result = std::decay_t<std::invoke_result_t<Work&, melonDS::NDS&>>;
        QPointer<QObject> guard(ctx);
        post([this, guard, work = std::move(work), done = std::move(done)](melonDS::NDS& nds) mutable {
            auto result = std::make_shared<Result>(work(nds));
            QMetaObject::invokeMethod(this, [guard, done, result]() mutable {
                if (guard)
                    done(*result);
            }, Qt::QueuedConnection);
        });
    }

    void adopt(QWidget* window);

    void attachConsole(melonDS::NDS* nds);
    void detachConsole();
    void serviceFrame();
    void idle(int ms);
    void wake();
    bool onEmuThread() const;

    void requestBreak(int cpu);
    bool isTemp(int id) const;
    void resume();
    void step(int cpu, melonDS::u32 count = 1);
    void stepOver(int cpu);
    void stepOut(int cpu);
    void runTo(int cpu, melonDS::u32 addr);

    void retainCallTracking();
    void releaseCallTracking();
    bool callTracking() const { return CallUsers > 0; }

    bool isBroken() const { return Broken; }
    bool isRunning() const;
    bool hasConsole() const;
    bool debugMode() const;
    melonDS::BreakEvent lastBreak() const;

    static melonDS::u32 currentPC(melonDS::NDS& nds, int cpu);

signals:
    void broke(melonDS::BreakEvent ev);
    void resumed();
    void consoleChanged();
    void started();
    void pauseChanged(bool paused);
    void pointsChanged();
    void symbolsChanged();

private:
    struct SavedPoint
    {
        bool Exec;
        melonDS::u8 CPUMask;
        melonDS::u8 Flags;
        bool Enabled;
        melonDS::u32 Start;
        melonDS::u32 Size;
    };

    EmuInstance* Inst;
    std::unique_ptr<DebugSymbols> Symbols;
    std::vector<QPointer<QWidget>> Windows;

    mutable std::mutex ConsoleLock;
    melonDS::NDS* Console = nullptr;

    std::mutex JobLock;
    std::condition_variable JobCond;
    std::deque<Job> Jobs;
    bool Woken = false;

    mutable std::mutex BreakLock;
    melonDS::BreakEvent Last {};
    std::atomic<bool> Broken {false};
    bool ResumeRequested = false;
    bool Detaching = false;
    std::vector<int> Temps;
    std::vector<SavedPoint> Saved;
    std::atomic<int> CallUsers {0};

    void onBreak(const melonDS::BreakEvent& ev);
    void runJobs();
    void clearTemps(melonDS::NDS& nds);
    void unpauseEmu();
    void addTemp(melonDS::NDS& nds, int cpu, melonDS::u32 addr, melonDS::u32 minSP, melonDS::u8 mode);
};

Q_DECLARE_METATYPE(melonDS::BreakEvent)

#endif
