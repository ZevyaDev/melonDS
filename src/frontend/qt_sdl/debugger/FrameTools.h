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

#ifndef FRAMETOOLS_H
#define FRAMETOOLS_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QString>

#include "Movie.h"
#include "MediaWriters.h"

namespace melonDS { class NDS; }
class EmuInstance;
class DebugBridge;
class QPainter;

struct InputPatch
{
    melonDS::u16 Press = 0;
    melonDS::u16 Release = 0;
    int Touch = -1;
    melonDS::u8 X = 0;
    melonDS::u8 Y = 0;
    int Lid = -1;
};

struct HudFlags
{
    bool FPS = false;
    bool Input = false;
    bool GraphicalInput = false;
    bool FrameCounter = false;
    bool LagCounter = false;
    bool RTC = false;
    bool Mic = false;

    bool any() const { return FPS || Input || GraphicalInput || FrameCounter || LagCounter || RTC || Mic; }
};

class FrameTools
{
public:
    FrameTools(EmuInstance* inst, DebugBridge* bridge);
    ~FrameTools();

    Movie& movie() { return MovieState; }

    void beforeFrame(FrameInput& input);
    void afterFrame();
    void onReset();

    FrameInput userInput() const;
    FrameInput lastInput() const;
    void patchInput(const InputPatch& patch);
    melonDS::u32 frameCount() const;
    melonDS::u32 lagCount() const;
    bool lagged() const;

    struct HostInput
    {
        std::vector<int> Keys;
        int X = -1;
        int Y = -1;
        bool Left = false;
        bool Right = false;
        bool Middle = false;
    };
    HostInput hostInput() const;
    void setHostInput(const HostInput& input);

    bool atBoundary() const;
    void atNextFrame(std::function<void()> fn);
    bool emulateFrame();
    bool saveStateFile(const std::string& path);
    bool loadStateFile(const std::string& path);
    std::string slotPath(int slot);

    void setLuaHook(const void* owner, std::function<void(bool)> hook);
    void paintLua(const std::function<void(QPainter&)>& paint);
    QRgb luaPixel(int x, int y);
    void clearLua();

    bool overlay(int screen, QImage& out, quint64& version) const;

    HudFlags hud() const;
    void setHud(const HudFlags& flags);

    melonDS::u8 hiddenLayers(int engine) const;
    void setHiddenLayers(int engine, melonDS::u8 mask);

    bool startAVI(const QString& path, AVICodec codec, QString& error);
    void stopAVI();
    bool startWAV(const QString& path, QString& error);
    void stopWAV();
    bool aviActive() const;
    bool wavActive() const;

    void requestScreenshot(const QString& path);
    void bootWithOverrides(const QString& dir);
    static bool grabScreens(melonDS::NDS& nds, QImage& out);

    static constexpr int AudioRate = 32728;
    static constexpr melonDS::u32 FrameRateNum = 33513982;
    static constexpr melonDS::u32 FrameRateDen = 560190;

private:
    EmuInstance* Inst;
    DebugBridge* Bridge;
    Movie MovieState;

    mutable std::mutex InputLock;
    FrameInput User;
    FrameInput Last;
    InputPatch Patch;
    bool HasPatch = false;
    bool Lagged = false;

    std::mutex HookLock;
    std::vector<std::pair<const void*, std::shared_ptr<std::function<void(bool)>>>> LuaHooks;
    std::vector<std::function<void()>> Pending;
    std::atomic<bool> Boundary {false};
    HostInput Host;

    std::mutex CanvasLock;
    QImage LuaCanvas;
    bool LuaCanvasUsed = false;
    mutable std::mutex OverlayLock;
    QImage Overlay;
    quint64 OverlayVersion = 0;
    bool OverlayEmpty = true;

    mutable std::mutex HudLock;
    HudFlags Hud;
    melonDS::u8 HiddenLayers[2] = {0, 0};
    QElapsedTimer FPSTimer;
    int FPSFrames = 0;
    int FPS = 0;

    mutable std::mutex RecordLock;
    std::unique_ptr<AVIWriter> AVI;
    std::unique_ptr<WAVWriter> WAV;
    std::vector<melonDS::s16> Audio;
    QString PendingShot;

    void runLuaHook(bool after);
    void runPending();
    void applyPatch(FrameInput& input);
    void applyInput(melonDS::NDS& nds, const FrameInput& input);
    void compose(melonDS::NDS& nds);
    void drawHud(QPainter& painter, melonDS::NDS& nds, const HudFlags& flags);
    void recordFrame(melonDS::NDS& nds);
    void takeScreenshot(melonDS::NDS& nds);
};

#endif
