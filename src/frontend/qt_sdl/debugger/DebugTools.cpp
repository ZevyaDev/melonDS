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

#include <QMenu>

#include "DebugTools.h"
#include "DebugBridge.h"
#include "DebuggerWindow.h"
#include "TraceWindow.h"
#include "RamSearchWindow.h"
#include "IORegsWindow.h"
#include "Gfx2DWindows.h"
#include "Gfx3DWindow.h"
#include "SoundWindow.h"
#include "DebugSymbols.h"
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
    };
    return tools;
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
    if (path.isEmpty() || path == bridge->symbols().source())
        return;
    QString error;
    if (bridge->symbols().load(path, error))
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
}

}
