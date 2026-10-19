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

#ifndef DEBUGTOOLS_H
#define DEBUGTOOLS_H

#include <QMap>
#include <QPointer>
#include <QWidget>

#include "types.h"

class QMenu;
class MainWindow;
class DebugBridge;

namespace DebugTools
{

void buildMenu(QMenu* menu, MainWindow* win);
void showDisassembly(DebugBridge* bridge, QWidget* parent, melonDS::u32 addr);
void autoLoadSymbols(DebugBridge* bridge);
void rememberSymbols(DebugBridge* bridge, const QString& path);
QWidget* openByName(const QString& name, DebugBridge* bridge, QWidget* parent);

template <typename T>
T* open(DebugBridge* bridge, QWidget* parent)
{
    static QMap<DebugBridge*, QPointer<T>> windows;
    QPointer<T>& win = windows[bridge];
    if (!win)
        win = new T(bridge, parent);
    win->show();
    win->raise();
    win->activateWindow();
    return win;
}

}

#endif
