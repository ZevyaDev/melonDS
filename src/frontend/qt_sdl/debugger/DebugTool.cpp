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

#include <QFontDatabase>

#include "DebugTool.h"

DebugTool::DebugTool(DebugBridge* bridge, const QString& title, QWidget* parent)
    : QWidget(parent, Qt::Window), Bridge(bridge)
{
    setAttribute(Qt::WA_DeleteOnClose);
    bridge->adopt(this);
    setWindowTitle(title);
    Timer = new QTimer(this);
    Timer->setInterval(250);
    connect(Timer, &QTimer::timeout, this, &DebugTool::onTimer);
    connect(bridge, &DebugBridge::broke, this, &DebugTool::requestRefresh);
    connect(bridge, &DebugBridge::pauseChanged, this, &DebugTool::requestRefresh);
    connect(bridge, &DebugBridge::consoleChanged, this, [this] {
        Pending = false;
        requestRefresh();
    });
    Since.start();
}

void DebugTool::setInterval(int ms)
{
    Timer->setInterval(ms);
}

void DebugTool::requestRefresh()
{
    if (isVisible() && Bridge->hasConsole())
        refresh();
}

void DebugTool::onTimer()
{
    if (Live && Bridge->isRunning())
        requestRefresh();
}

void DebugTool::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    Timer->start();
    QTimer::singleShot(0, this, &DebugTool::requestRefresh);
}

void DebugTool::hideEvent(QHideEvent* event)
{
    Timer->stop();
    QWidget::hideEvent(event);
}

QFont MonoFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    return f;
}

QString Hex(melonDS::u32 value, int digits)
{
    return QString("%1").arg(value, digits, 16, QChar('0')).toUpper();
}
