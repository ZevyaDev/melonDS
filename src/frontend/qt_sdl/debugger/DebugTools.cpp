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

#include <functional>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>

#include "DebugTools.h"
#include "DebugBridge.h"
#include "DebuggerWindow.h"
#include "Gfx3DStateWindows.h"
#include "NitroFSWindow.h"
#include "LogWindow.h"
#include "FrameTools.h"
#include "MemViewDialog.h"
#include "TraceWindow.h"
#include "RamSearchWindow.h"
#include "IORegsWindow.h"
#include "Gfx2DWindows.h"
#include "Gfx3DWindow.h"
#include "SoundWindow.h"
#include "DebugSymbolsQt.h"
#include "EmuInstance.h"
#include "Config.h"
#include "NDSCart.h"
#include "Window.h"

namespace DebugTools
{

struct Tool
{
    const char* Name;
    const char* Label;
    std::function<QWidget*(DebugBridge*, QWidget*)> Open;
};

static const std::vector<Tool>& Tools()
{
    static const std::vector<Tool> tools = {
        {"debugger", "Debugger", [](DebugBridge* b, QWidget* p) -> QWidget* { return DebuggerWindow::open(b, p); }},
        {"trace", "Trace", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<TraceWindow>(b, p); }},
        {"profiler", "Call counts and coverage", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<ProfilerWindow>(b, p); }},
        {"lastwriter", "Last writer", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<LastWriterWindow>(b, p); }},
        {"ramsearch", "RAM search", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<RamSearchWindow>(b, p); }},
        {"ramwatch", "RAM watch", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<RamWatchWindow>(b, p); }},
        {"ioregs", "IO registers", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<IORegsWindow>(b, p); }},
        {"palette", "Palettes", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<PaletteWindow>(b, p); }},
        {"tiles", "Tile viewer", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<TileWindow>(b, p); }},
        {"bgmap", "BG map", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<BGMapWindow>(b, p); }},
        {"oam", "OAM / sprites", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<OAMWindow>(b, p); }},
        {"gfx3d", "3D graphics", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<Gfx3DWindow>(b, p); }},
        {"sound", "Sound", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<SoundWindow>(b, p); }},
        {"matrices", "Matrices", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<MatrixWindow>(b, p); }},
        {"lights", "Lights", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<LightWindow>(b, p); }},
        {"nitrofs", "NitroFS", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<NitroFSWindow>(b, p); }},
        {"log", "Log", [](DebugBridge* b, QWidget* p) -> QWidget* { return open<LogWindow>(b, p); }},
    };
    return tools;
}

QString gameCode(DebugBridge* bridge);

void showMemory(DebugBridge* bridge, QWidget* parent, MemSpace space, melonDS::u32 addr)
{
    MemViewDialog* dlg = MemViewDialog::openDlg(bridge->instance()->getMainWindow());
    dlg->show();
    dlg->raise();
    dlg->SetSpace(space);
    dlg->GoToAddress(Hex(addr));
}

static QString ScreenshotPath(DebugBridge* bridge)
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/melonDS";
    QDir().mkpath(dir);
    QString code = gameCode(bridge);
    return dir + "/" + (code.isEmpty() ? QString("melonDS") : code) + "_"
        + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz") + ".png";
}

static void RecordMovie(DebugBridge* bridge, QWidget* win, bool powerOn)
{
    QString path = QFileDialog::getSaveFileName(win, powerOn ? "Record movie from power-on" : "Record movie from now",
        QString(), "Movies (*.dsm)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".dsm", Qt::CaseInsensitive))
        path += ".dsm";
    bool ok = false;
    QString author = QInputDialog::getText(win, "Record movie", "Author:", QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;
    bridge->frames().movie().requestRecord(path, author, powerOn);
}

static void StartAVI(DebugBridge* bridge, QWidget* win, AVICodec codec)
{
    QString path = QFileDialog::getSaveFileName(win, "Record AVI", QString(), "AVI files (*.avi)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".avi", Qt::CaseInsensitive))
        path += ".avi";
    QString error;
    if (!bridge->frames().startAVI(path, codec, error))
        QMessageBox::warning(win, "Record AVI", error);
}

static void StartWAV(DebugBridge* bridge, QWidget* win)
{
    QString path = QFileDialog::getSaveFileName(win, "Record WAV", QString(), "WAV files (*.wav)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".wav", Qt::CaseInsensitive))
        path += ".wav";
    QString error;
    if (!bridge->frames().startWAV(path, error))
        QMessageBox::warning(win, "Record WAV", error);
}

static void BuildMediaMenus(QMenu* menu, MainWindow* win, DebugBridge* bridge)
{
    QMenu* movie = menu->addMenu("Movie");
    movie->addAction("Record from power-on...", win, [bridge, win] { RecordMovie(bridge, win, true); });
    movie->addAction("Record from now...", win, [bridge, win] { RecordMovie(bridge, win, false); });
    movie->addAction("Play...", win, [bridge, win] {
        QString path = QFileDialog::getOpenFileName(win, "Play movie", QString(), "Movies (*.dsm)");
        if (!path.isEmpty())
            bridge->frames().movie().requestPlay(path, bridge->frames().movie().status().ReadOnly);
    });
    QAction* stop = movie->addAction("Stop", win, [bridge] { bridge->frames().movie().requestStop(); });
    QAction* readOnly = movie->addAction("Read-only");
    readOnly->setCheckable(true);
    readOnly->setChecked(true);
    QObject::connect(readOnly, &QAction::toggled, win, [bridge](bool on) { bridge->frames().movie().setReadOnly(on); });
    QAction* rerecords = movie->addAction("Set rerecord count...", win, [bridge, win] {
        bool ok = false;
        int count = QInputDialog::getInt(win, "Movie", "Rerecord count:", bridge->frames().movie().status().Rerecords, 0, 0x7FFFFFFF, 1, &ok);
        if (ok)
            bridge->frames().movie().setRerecords(count);
    });
    QObject::connect(movie, &QMenu::aboutToShow, win, [bridge, stop, readOnly, rerecords] {
        Movie::Status s = bridge->frames().movie().status();
        stop->setEnabled(s.State != Movie::Inactive);
        rerecords->setEnabled(s.State != Movie::Inactive);
        QSignalBlocker block(readOnly);
        readOnly->setChecked(s.ReadOnly);
    });

    QMenu* record = menu->addMenu("Record");
    record->addAction("AVI (MJPEG)...", win, [bridge, win] { StartAVI(bridge, win, AVICodec::MJPEG); });
    record->addAction("AVI (uncompressed)...", win, [bridge, win] { StartAVI(bridge, win, AVICodec::Uncompressed); });
    QAction* stopAVI = record->addAction("Stop AVI", win, [bridge] { bridge->frames().stopAVI(); });
    record->addSeparator();
    record->addAction("WAV...", win, [bridge, win] { StartWAV(bridge, win); });
    QAction* stopWAV = record->addAction("Stop WAV", win, [bridge] { bridge->frames().stopWAV(); });
    record->addSeparator();
    record->addAction("Screenshot", win, [bridge] { bridge->frames().requestScreenshot(ScreenshotPath(bridge)); });
    record->addAction("Screenshot as...", win, [bridge, win] {
        QString path = QFileDialog::getSaveFileName(win, "Save screenshot", QString(), "PNG images (*.png);;BMP images (*.bmp)");
        if (!path.isEmpty())
            bridge->frames().requestScreenshot(path);
    });
    QObject::connect(record, &QMenu::aboutToShow, win, [bridge, stopAVI, stopWAV] {
        stopAVI->setEnabled(bridge->frames().aviActive());
        stopWAV->setEnabled(bridge->frames().wavActive());
    });

    menu->addAction("Boot with NitroFS overrides...", win, [bridge, win] {
        QString dir = QFileDialog::getExistingDirectory(win, "Folder with replacement NitroFS files");
        if (!dir.isEmpty())
            bridge->frames().bootWithOverrides(dir);
    });

    QMenu* layers = menu->addMenu("View layers");
    static const char* const engines[2] = {"Main engine (A)", "Sub engine (B)"};
    static const char* const names[5] = {"BG0 / 3D", "BG1", "BG2", "BG3", "OBJ"};
    std::vector<std::pair<int, QAction*>> layerActions;
    for (int engine = 0; engine < 2; engine++)
    {
        QMenu* sub = layers->addMenu(engines[engine]);
        for (int layer = 0; layer < 5; layer++)
        {
            QAction* act = sub->addAction(engine ? QString(names[layer]).replace(" / 3D", "") : QString(names[layer]));
            act->setCheckable(true);
            act->setChecked(true);
            QObject::connect(act, &QAction::toggled, win, [bridge, engine, layer](bool shown) {
                FrameTools& f = bridge->frames();
                melonDS::u8 mask = f.hiddenLayers(engine);
                f.setHiddenLayers(engine, shown ? mask & ~(1 << layer) : mask | (1 << layer));
            });
            layerActions.push_back({engine * 8 + layer, act});
        }
        sub->addSeparator();
        sub->addAction("Hide all", win, [bridge, engine] { bridge->frames().setHiddenLayers(engine, 0x1F); });
        sub->addAction("Show all", win, [bridge, engine] { bridge->frames().setHiddenLayers(engine, 0); });
    }
    QObject::connect(layers, &QMenu::aboutToShow, win, [bridge, layerActions] {
        for (const auto& [key, act] : layerActions)
        {
            QSignalBlocker block(act);
            act->setChecked(!(bridge->frames().hiddenLayers(key >> 3) & (1 << (key & 7))));
        }
    });
    for (QMenu* sub : layers->findChildren<QMenu*>())
        QObject::connect(sub, &QMenu::aboutToShow, layers, &QMenu::aboutToShow);

    QMenu* hud = menu->addMenu("HUD");
    static const char* const hudNames[7] = {"FPS", "Input", "Graphical input", "Frame counter", "Lag counter", "RTC", "Microphone"};
    std::vector<QAction*> hudActions;
    for (int i = 0; i < 7; i++)
    {
        QAction* act = hud->addAction(hudNames[i]);
        act->setCheckable(true);
        hudActions.push_back(act);
        QObject::connect(act, &QAction::toggled, win, [bridge, i](bool on) {
            HudFlags flags = bridge->frames().hud();
            bool* fields[7] = {&flags.FPS, &flags.Input, &flags.GraphicalInput, &flags.FrameCounter, &flags.LagCounter, &flags.RTC, &flags.Mic};
            *fields[i] = on;
            bridge->frames().setHud(flags);
        });
    }
    QObject::connect(hud, &QMenu::aboutToShow, win, [bridge, hudActions] {
        HudFlags flags = bridge->frames().hud();
        bool values[7] = {flags.FPS, flags.Input, flags.GraphicalInput, flags.FrameCounter, flags.LagCounter, flags.RTC, flags.Mic};
        for (int i = 0; i < 7; i++)
        {
            QSignalBlocker block(hudActions[i]);
            hudActions[i]->setChecked(values[i]);
        }
    });
}

QString gameCode(DebugBridge* bridge)
{
    melonDS::NDS* nds = bridge->instance()->getNDS();
    if (!nds || !nds->GetNDSCart())
        return QString();
    return QString::fromLatin1(nds->GetNDSCart()->GetHeader().GameCode, 4);
}

void autoLoadSymbols(DebugBridge* bridge)
{
    QString code = gameCode(bridge);
    if (code.isEmpty())
        return;
    Config::Table& cfg = bridge->instance()->getGlobalConfig();
    QString path = QString::fromStdString(cfg.GetString("Debugger.Symbols." + code.toStdString()));
    QString labels = QString::fromStdString(cfg.GetString("Debugger.Labels." + code.toStdString()));
    DebugSymbols& symbols = bridge->symbols();
    bool changed = false;
    QString error;
    if (!path.isEmpty() && (SymStr(DebugSource::Normalize(path)) != symbols.source() || symbols.needsRom()))
        changed |= LoadSymbols(symbols, path, error, bridge->instance()->getNDS());
    if (labels != SymStr(symbols.labelsSource()))
    {
        std::string labelError;
        if (labels.isEmpty())
        {
            symbols.clearLabels();
            changed = true;
        }
        else if (symbols.loadLabels(SymStr(labels), labelError))
            changed = true;
    }
    if (changed)
        emit bridge->symbolsChanged();
}

void rememberSymbols(DebugBridge* bridge, const QString& path)
{
    QString code = gameCode(bridge);
    if (code.isEmpty())
        return;
    Config::Table& cfg = bridge->instance()->getGlobalConfig();
    cfg.SetString("Debugger.Symbols." + code.toStdString(), path.toStdString());
    Config::Save();
}

void rememberLabels(DebugBridge* bridge, const QString& path)
{
    QString code = gameCode(bridge);
    if (code.isEmpty())
        return;
    Config::Table& cfg = bridge->instance()->getGlobalConfig();
    cfg.SetString("Debugger.Labels." + code.toStdString(), path.toStdString());
    Config::Save();
}

void showDisassembly(DebugBridge* bridge, QWidget* parent, melonDS::u32 addr)
{
    DebuggerWindow::open(bridge, parent)->goTo(addr);
}

QWidget* openByName(const QString& name, DebugBridge* bridge, QWidget* parent)
{
    for (const Tool& t : Tools())
        if (name == t.Name)
            return t.Open(bridge, parent);
    return nullptr;
}

void buildMenu(QMenu* menu, MainWindow* win)
{
    DebugBridge* bridge = win->getEmuInstance()->getDebugBridge();
    static QSet<DebugBridge*> watched;
    if (!watched.contains(bridge))
    {
        watched.insert(bridge);
        QObject::connect(bridge, &DebugBridge::broke, bridge, [bridge] {
            DebuggerWindow::open(bridge, bridge->instance()->getMainWindow());
        });
        QObject::connect(bridge, &DebugBridge::started, bridge, [bridge] { autoLoadSymbols(bridge); });
        QObject::connect(bridge, &DebugBridge::consoleChanged, bridge, [bridge] { autoLoadSymbols(bridge); });
    }
    for (const Tool& t : Tools())
    {
        auto open = t.Open;
        QAction* act = menu->addAction(t.Label, win, [open, bridge, win] { open(bridge, win); });
        if (QString(t.Name) == "debugger")
            act->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_D);
    }
    menu->addSeparator();
    BuildMediaMenus(menu, win, bridge);
}

}
