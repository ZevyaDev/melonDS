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
#include <cstring>

#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>

#include "FrameTools.h"
#include "NitroOverride.h"
#include "DebugBridge.h"
#include "NDSCart.h"
#include "EmuInstance.h"
#include "Config.h"
#include "NDS.h"
#include "GPU.h"
#include "SPU.h"
#include "RTC.h"

#ifdef OGLRENDERER_ENABLED
#include "OpenGLSupport.h"
#endif

using namespace melonDS;

static const char* const HudKeys[7] = {"FPS", "Input", "GraphicalInput", "FrameCounter", "LagCounter", "RTC", "Mic"};

static bool* HudField(HudFlags& flags, int i)
{
    bool* fields[7] = {&flags.FPS, &flags.Input, &flags.GraphicalInput, &flags.FrameCounter, &flags.LagCounter, &flags.RTC, &flags.Mic};
    return fields[i];
}

FrameTools::FrameTools(EmuInstance* inst, DebugBridge* bridge) : Inst(inst), Bridge(bridge), MovieState(inst)
{
    LuaCanvas = QImage(256, 384, QImage::Format_ARGB32_Premultiplied);
    LuaCanvas.fill(Qt::transparent);
    Config::Table& cfg = inst->getGlobalConfig();
    for (int i = 0; i < 7; i++)
        *HudField(Hud, i) = cfg.GetBool(std::string("Debugger.HUD.") + HudKeys[i]);
    FPSTimer.start();
}

FrameTools::~FrameTools()
{
    stopAVI();
    stopWAV();
}

void FrameTools::applyPatch(FrameInput& input)
{
    std::lock_guard lock(InputLock);
    if (!HasPatch)
        return;
    input.Keys = (input.Keys | Patch.Press) & ~Patch.Release;
    if (Patch.Touch == 0)
        input.Touching = false;
    else if (Patch.Touch == 1)
    {
        input.Touching = true;
        input.TouchX = Patch.X;
        input.TouchY = Patch.Y;
    }
    if (Patch.Lid != -1)
        input.Lid = Patch.Lid;
    Patch = InputPatch();
    HasPatch = false;
}

void FrameTools::applyInput(NDS& nds, const FrameInput& input)
{
    nds.SetKeyMask(~input.Keys & 0xFFF);
    if (input.Touching)
        nds.TouchScreen(input.TouchX, input.TouchY);
    else
        nds.ReleaseScreen();
    if (input.Lid != nds.IsLidClosed())
        nds.SetLidClosed(input.Lid);
}

void FrameTools::runPending()
{
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard lock(HookLock);
        pending.swap(Pending);
    }
    if (pending.empty())
        return;
    Boundary = true;
    for (auto& fn : pending)
        fn();
    Boundary = false;
}

void FrameTools::atNextFrame(std::function<void()> fn)
{
    {
        std::lock_guard lock(HookLock);
        Pending.push_back(std::move(fn));
    }
    if (!Bridge->isRunning() && !Bridge->isBroken())
        Bridge->post([this](NDS&) { runPending(); });
}

bool FrameTools::atBoundary() const
{
    return Boundary && Bridge->onEmuThread();
}

bool FrameTools::emulateFrame()
{
    NDS* nds = Inst->getNDS();
    if (!nds || !atBoundary())
        return false;
    FrameInput input = userInput();
    applyPatch(input);
    applyInput(*nds, input);
    Boundary = false;
    nds->RunFrame();
    Boundary = true;
    std::lock_guard lock(InputLock);
    Last = input;
    Lagged = nds->LagFrameFlag;
    return true;
}

bool FrameTools::saveStateFile(const std::string& path)
{
    return atBoundary() && Inst->saveState(path);
}

bool FrameTools::loadStateFile(const std::string& path)
{
    return atBoundary() && Inst->loadState(path);
}

std::string FrameTools::slotPath(int slot)
{
    return Inst->getSavestateName(slot);
}

FrameTools::HostInput FrameTools::hostInput() const
{
    std::lock_guard lock(InputLock);
    return Host;
}

void FrameTools::setHostInput(const HostInput& input)
{
    std::lock_guard lock(InputLock);
    Host = input;
}

void FrameTools::beforeFrame(FrameInput& input)
{
    runPending();
    {
        std::lock_guard lock(InputLock);
        User = input;
    }
    runLuaHook(false);
    applyPatch(input);
    MovieState.beforeFrame(input);
    if (NDS* nds = Inst->getNDS())
    {
        {
            std::lock_guard lock(HudLock);
            nds->GPU.GPU2D_A.DebugHiddenLayers = HiddenLayers[0];
            nds->GPU.GPU2D_B.DebugHiddenLayers = HiddenLayers[1];
        }
        std::lock_guard lock(RecordLock);
        nds->SPU.DebugCapture = (AVI || WAV) ? &Audio : nullptr;
    }
    std::lock_guard lock(InputLock);
    Last = input;
}

void FrameTools::afterFrame()
{
    NDS* nds = Inst->getNDS();
    if (!nds)
        return;
    recordFrame(*nds);
    takeScreenshot(*nds);
    MovieState.afterFrame(*nds);
    {
        std::lock_guard lock(InputLock);
        Lagged = nds->LagFrameFlag;
    }
    FPSFrames++;
    qint64 elapsed = FPSTimer.elapsed();
    if (elapsed >= 1000)
    {
        FPS = (int)(FPSFrames * 1000 / elapsed);
        FPSFrames = 0;
        FPSTimer.restart();
    }
    runLuaHook(true);
    compose(*nds);
}

void FrameTools::onReset()
{
    MovieState.onReset();
}

FrameInput FrameTools::userInput() const
{
    std::lock_guard lock(InputLock);
    return User;
}

FrameInput FrameTools::lastInput() const
{
    std::lock_guard lock(InputLock);
    return Last;
}

void FrameTools::patchInput(const InputPatch& patch)
{
    std::lock_guard lock(InputLock);
    if (!HasPatch)
        Patch = InputPatch();
    Patch.Press = (Patch.Press & ~patch.Release) | patch.Press;
    Patch.Release = (Patch.Release & ~patch.Press) | patch.Release;
    if (patch.Touch != -1)
    {
        Patch.Touch = patch.Touch;
        Patch.X = patch.X;
        Patch.Y = patch.Y;
    }
    if (patch.Lid != -1)
        Patch.Lid = patch.Lid;
    HasPatch = true;
}

u32 FrameTools::frameCount() const
{
    Movie::Status movie = MovieState.status();
    if (movie.State != Movie::Inactive)
        return movie.Frame;
    NDS* nds = Inst->getNDS();
    return nds ? nds->NumFrames : 0;
}

u32 FrameTools::lagCount() const
{
    NDS* nds = Inst->getNDS();
    return nds ? nds->NumLagFrames : 0;
}

bool FrameTools::lagged() const
{
    std::lock_guard lock(InputLock);
    return Lagged;
}

void FrameTools::setLuaHook(const void* owner, std::function<void(bool)> hook)
{
    std::lock_guard lock(HookLock);
    LuaHooks.erase(std::remove_if(LuaHooks.begin(), LuaHooks.end(), [owner](const auto& h) { return h.first == owner; }), LuaHooks.end());
    if (hook)
        LuaHooks.push_back({owner, std::make_shared<std::function<void(bool)>>(std::move(hook))});
}

void FrameTools::runLuaHook(bool after)
{
    std::vector<std::shared_ptr<std::function<void(bool)>>> hooks;
    {
        std::lock_guard lock(HookLock);
        for (const auto& h : LuaHooks)
            hooks.push_back(h.second);
    }
    if (hooks.empty())
        return;
    Boundary = true;
    for (const auto& hook : hooks)
        (*hook)(after);
    Boundary = false;
}

void FrameTools::paintLua(const std::function<void(QPainter&)>& paint)
{
    std::lock_guard lock(CanvasLock);
    QPainter painter(&LuaCanvas);
    paint(painter);
    LuaCanvasUsed = true;
}

QRgb FrameTools::luaPixel(int x, int y)
{
    std::lock_guard lock(CanvasLock);
    if (x < 0 || y < 0 || x >= LuaCanvas.width() || y >= LuaCanvas.height())
        return 0;
    return LuaCanvas.pixel(x, y);
}

void FrameTools::clearLua()
{
    std::lock_guard lock(CanvasLock);
    LuaCanvas.fill(Qt::transparent);
    LuaCanvasUsed = true;
}

bool FrameTools::overlay(int screen, QImage& out, quint64& version) const
{
    std::lock_guard lock(OverlayLock);
    version = OverlayVersion;
    if (OverlayEmpty)
        return false;
    out = Overlay.copy(0, 192 * (screen & 1), 256, 192);
    return true;
}

HudFlags FrameTools::hud() const
{
    std::lock_guard lock(HudLock);
    return Hud;
}

void FrameTools::setHud(const HudFlags& flags)
{
    {
        std::lock_guard lock(HudLock);
        Hud = flags;
    }
    Config::Table& cfg = Inst->getGlobalConfig();
    HudFlags copy = flags;
    for (int i = 0; i < 7; i++)
        cfg.SetBool(std::string("Debugger.HUD.") + HudKeys[i], *HudField(copy, i));
    Config::Save();
}

u8 FrameTools::hiddenLayers(int engine) const
{
    std::lock_guard lock(HudLock);
    return HiddenLayers[engine & 1];
}

void FrameTools::setHiddenLayers(int engine, u8 mask)
{
    std::lock_guard lock(HudLock);
    HiddenLayers[engine & 1] = mask & 0x1F;
}

static void OutlinedText(QPainter& painter, int x, int y, const QString& text, QColor color = Qt::white)
{
    QPainterPath path;
    path.addText(x, y, painter.font(), text);
    painter.setPen(QPen(QColor(0, 0, 0, 220), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.fillPath(path, color);
}

static QString InputText(const FrameInput& in)
{
    static const char* names = "RLDUTSBAYXWE";
    static const int keyOf[12] = {4, 5, 7, 6, 3, 2, 1, 0, 11, 10, 9, 8};
    QString text;
    for (int i = 0; i < 12; i++)
        text += (in.Keys & (1 << keyOf[i])) ? QChar(names[i]) : QChar('.');
    if (in.Touching)
        text += QString(" %1,%2").arg(in.TouchX).arg(in.TouchY);
    if (in.Lid)
        text += " lid";
    return text;
}

static void DrawButton(QPainter& painter, const QRectF& rect, bool down, bool round, const QString& label)
{
    painter.setPen(QPen(QColor(0, 0, 0, 200), 1));
    painter.setBrush(down ? QColor(255, 255, 255, 230) : QColor(60, 60, 60, 150));
    if (round)
        painter.drawEllipse(rect);
    else
        painter.drawRect(rect);
    if (!label.isEmpty())
    {
        painter.setPen(down ? Qt::black : QColor(220, 220, 220));
        painter.drawText(rect, Qt::AlignCenter, label);
    }
}

static void DrawPad(QPainter& painter, const FrameInput& in, int ox, int oy)
{
    auto down = [&](int key) { return (in.Keys & (1 << key)) != 0; };
    QFont font = painter.font();
    QFont small = font;
    small.setPixelSize(6);
    painter.setFont(small);
    DrawButton(painter, QRectF(ox + 8, oy, 8, 8), down(6), false, "");
    DrawButton(painter, QRectF(ox + 8, oy + 16, 8, 8), down(7), false, "");
    DrawButton(painter, QRectF(ox, oy + 8, 8, 8), down(5), false, "");
    DrawButton(painter, QRectF(ox + 16, oy + 8, 8, 8), down(4), false, "");
    DrawButton(painter, QRectF(ox + 30, oy + 12, 10, 6), down(2), false, "");
    DrawButton(painter, QRectF(ox + 42, oy + 12, 10, 6), down(3), false, "");
    DrawButton(painter, QRectF(ox + 64, oy, 9, 9), down(10), true, "X");
    DrawButton(painter, QRectF(ox + 56, oy + 8, 9, 9), down(11), true, "Y");
    DrawButton(painter, QRectF(ox + 72, oy + 8, 9, 9), down(0), true, "A");
    DrawButton(painter, QRectF(ox + 64, oy + 16, 9, 9), down(1), true, "B");
    DrawButton(painter, QRectF(ox, oy - 10, 16, 6), down(9), false, "L");
    DrawButton(painter, QRectF(ox + 65, oy - 10, 16, 6), down(8), false, "R");
    painter.setFont(font);
}

void FrameTools::drawHud(QPainter& painter, NDS& nds, const HudFlags& flags)
{
    QFont font("Courier New");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(9);
    font.setBold(true);
    painter.setFont(font);
    painter.setRenderHint(QPainter::Antialiasing, false);
    FrameInput in = lastInput();
    int y = 10;
    auto line = [&](const QString& text, QColor color = Qt::white) {
        OutlinedText(painter, 3, y, text, color);
        y += 10;
    };
    if (flags.FPS)
        line(QString("FPS %1").arg(FPS));
    Movie::Status movie = MovieState.status();
    if (flags.FrameCounter)
    {
        QString text = QString::number(frameCount());
        if (movie.State == Movie::Recording)
            text += QString(" rec %1").arg(movie.Length);
        else if (movie.State == Movie::Playing)
            text += QString("/%1 play%2").arg(movie.Length).arg(movie.ReadOnly ? "" : " rw");
        line(text, movie.State == Movie::Recording ? QColor(255, 120, 120) : Qt::white);
    }
    if (flags.LagCounter)
        line(QString("lag %1").arg(movie.State != Movie::Inactive ? movie.Lag : lagCount()), lagged() ? QColor(255, 80, 80) : Qt::white);
    if (flags.Input)
        line(InputText(in));
    if (flags.RTC)
    {
        int year, month, day, hour, minute, second;
        nds.RTC.GetDateTime(year, month, day, hour, minute, second);
        line(QString::asprintf("%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second));
    }
    if (flags.Mic)
        line(in.Mic ? "mic: blow" : "mic: -");
    if (flags.GraphicalInput)
    {
        DrawPad(painter, in, 168, 192 + 160);
        if (in.Touching)
        {
            painter.setPen(QPen(QColor(255, 0, 0, 220), 1));
            int tx = in.TouchX, ty = 192 + in.TouchY;
            painter.drawLine(tx - 4, ty, tx + 4, ty);
            painter.drawLine(tx, ty - 4, tx, ty + 4);
        }
    }
}

void FrameTools::compose(NDS& nds)
{
    HudFlags flags = hud();
    bool used;
    {
        std::lock_guard lock(CanvasLock);
        used = LuaCanvasUsed;
    }
    if (!used && !flags.any())
    {
        std::lock_guard lock(OverlayLock);
        if (!OverlayEmpty)
        {
            Overlay = QImage();
            OverlayEmpty = true;
            OverlayVersion++;
        }
        return;
    }
    QImage image(256, 384, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    if (used)
    {
        std::lock_guard lock(CanvasLock);
        painter.drawImage(0, 0, LuaCanvas);
        LuaCanvas.fill(Qt::transparent);
        LuaCanvasUsed = false;
    }
    if (flags.any())
        drawHud(painter, nds, flags);
    painter.end();
    std::lock_guard lock(OverlayLock);
    Overlay = image;
    OverlayEmpty = false;
    OverlayVersion++;
}

bool FrameTools::grabScreens(NDS& nds, QImage& out)
{
    void* top = nullptr;
    void* bottom = nullptr;
    out = QImage(256, 384, QImage::Format_RGB32);
    if (nds.GPU.GetFramebuffers(&top, &bottom))
    {
        if (!top || !bottom)
            return false;
        memcpy(out.scanLine(0), top, 256 * 192 * 4);
        memcpy(out.scanLine(192), bottom, 256 * 192 * 4);
        return true;
    }
#ifdef OGLRENDERER_ENABLED
    if (!top)
        return false;
    GLuint texture = *(GLuint*)top;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previous);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    GLint width = 0, height = 0, layers = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &layers);
    if (width < 256 || height < 192 || layers < 2)
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, previous);
        return false;
    }
    std::vector<u32> pixels((size_t)width * height * layers);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, previous);
    int sx = width / 256, sy = height / 192;
    for (int screen = 0; screen < 2; screen++)
    {
        const u32* layer = pixels.data() + (size_t)screen * width * height;
        for (int y = 0; y < 192; y++)
        {
            u32* dst = (u32*)out.scanLine(screen * 192 + y);
            const u32* src = layer + (size_t)(y * sy) * width;
            for (int x = 0; x < 256; x++)
                dst[x] = src[x * sx] | 0xFF000000;
        }
    }
    return true;
#else
    return false;
#endif
}

void FrameTools::recordFrame(NDS& nds)
{
    std::lock_guard lock(RecordLock);
    if (!AVI && !WAV)
    {
        Audio.clear();
        return;
    }
    size_t frames = Audio.size() / 2;
    if (WAV)
    {
        WAV->write(Audio.data(), frames);
        if (!WAV->isOpen())
        {
            Inst->osdAddMessage(0, "WAV recording stopped: %s", WAV->error().toUtf8().constData());
            WAV.reset();
        }
    }
    if (AVI)
    {
        QImage image;
        if (grabScreens(nds, image))
            AVI->addFrame((const u32*)image.constBits(), Audio.data(), frames);
        if (!AVI->isOpen())
        {
            Inst->osdAddMessage(0, "AVI recording stopped: %s", AVI->error().toUtf8().constData());
            AVI.reset();
        }
    }
    Audio.clear();
}

bool FrameTools::startAVI(const QString& path, AVICodec codec, QString& error)
{
    auto writer = std::make_unique<AVIWriter>();
    if (!writer->open(path, 256, 384, FrameRateNum, FrameRateDen, AudioRate, codec))
    {
        error = writer->error();
        return false;
    }
    std::lock_guard lock(RecordLock);
    if (AVI)
        AVI->close();
    AVI = std::move(writer);
    return true;
}

void FrameTools::stopAVI()
{
    std::unique_ptr<AVIWriter> writer;
    {
        std::lock_guard lock(RecordLock);
        writer = std::move(AVI);
    }
    if (writer)
        writer->close();
}

bool FrameTools::startWAV(const QString& path, QString& error)
{
    auto writer = std::make_unique<WAVWriter>();
    if (!writer->open(path, AudioRate, 2))
    {
        error = writer->error();
        return false;
    }
    std::lock_guard lock(RecordLock);
    if (WAV)
        WAV->close();
    WAV = std::move(writer);
    return true;
}

void FrameTools::stopWAV()
{
    std::unique_ptr<WAVWriter> writer;
    {
        std::lock_guard lock(RecordLock);
        writer = std::move(WAV);
    }
    if (writer)
        writer->close();
}

bool FrameTools::aviActive() const
{
    std::lock_guard lock(RecordLock);
    return AVI != nullptr;
}

bool FrameTools::wavActive() const
{
    std::lock_guard lock(RecordLock);
    return WAV != nullptr;
}

void FrameTools::requestScreenshot(const QString& path)
{
    {
        std::lock_guard lock(RecordLock);
        PendingShot = path;
    }
    if (!Bridge->isRunning())
        Bridge->post([this](NDS& nds) { takeScreenshot(nds); });
}

void FrameTools::bootWithOverrides(const QString& dir)
{
    atNextFrame([this, dir] {
        NDS* nds = Inst->getNDS();
        const NDSCart::CartCommon* cart = nds ? nds->GetNDSCart() : nullptr;
        if (!cart || !cart->GetROM())
        {
            Inst->osdAddMessage(0, "NitroFS overrides need a loaded cartridge");
            return;
        }
        std::vector<u8> rom(cart->GetROM(), cart->GetROM() + cart->GetROMLength());
        QStringList replaced;
        QString error;
        if (!NitroOverride::Apply(rom, dir, replaced, error))
        {
            Inst->osdAddMessage(0, "NitroFS overrides: %s", error.toUtf8().constData());
            return;
        }
        NDSCart::NDSCartArgs args;
        u32 saveLength = cart->GetSaveMemoryLength();
        if (saveLength && cart->GetSaveMemory())
        {
            args.SRAM = std::make_unique<u8[]>(saveLength);
            memcpy(args.SRAM.get(), cart->GetSaveMemory(), saveLength);
            args.SRAMLength = saveLength;
        }
        auto data = std::make_unique<u8[]>(rom.size());
        memcpy(data.get(), rom.data(), rom.size());
        auto newCart = NDSCart::ParseROM(std::move(data), rom.size(), Inst, std::move(args));
        if (!newCart)
        {
            Inst->osdAddMessage(0, "NitroFS overrides: the rebuilt ROM did not parse");
            return;
        }
        nds->SetNDSCart(std::move(newCart));
        Inst->reset();
        Inst->osdAddMessage(0, "Booted with %d NitroFS overrides from %s", (int)replaced.size(), dir.toUtf8().constData());
    });
}

void FrameTools::takeScreenshot(NDS& nds)
{
    QString path;
    {
        std::lock_guard lock(RecordLock);
        path.swap(PendingShot);
    }
    if (path.isEmpty())
        return;
    QImage image;
    if (!grabScreens(nds, image) || !image.save(path))
    {
        Inst->osdAddMessage(0, "Screenshot failed: %s", path.toUtf8().constData());
        return;
    }
    Inst->osdAddMessage(0, "Screenshot saved: %s", QFileInfo(path).fileName().toUtf8().constData());
}
