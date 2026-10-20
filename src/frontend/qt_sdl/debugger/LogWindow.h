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

#ifndef LOGWINDOW_H
#define LOGWINDOW_H

#include <string>

#include "DebugTool.h"

class QCheckBox;
class QPlainTextEdit;

namespace DebugLog
{

enum Source
{
    source_Emulator,
    source_ARM9,
    source_ARM7,
};

void append(int instance, int source, const char* text, bool newline);

}

class LogWindow : public DebugTool
{
    Q_OBJECT

public:
    LogWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QPlainTextEdit* View;
    QCheckBox* ShowARM9;
    QCheckBox* ShowARM7;
    QCheckBox* ShowEmulator;
    QCheckBox* Frozen;
    unsigned long long Seen = 0;

    void reload();
};

#endif
