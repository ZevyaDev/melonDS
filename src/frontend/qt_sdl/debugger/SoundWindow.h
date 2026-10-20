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

#ifndef SOUNDWINDOW_H
#define SOUNDWINDOW_H

#include "DebugTool.h"

class QCheckBox;
class QLabel;
class QProgressBar;
class QTableWidget;

struct SoundChannelState
{
    melonDS::u32 Cnt;
    melonDS::u32 SrcAddr;
    melonDS::u16 TimerReload;
    melonDS::u32 LoopPos;
    melonDS::u32 Length;
    melonDS::u8 Volume;
    melonDS::u8 VolumeShift;
    melonDS::u8 Pan;
    melonDS::s32 Pos;
    melonDS::s16 CurSample;
    melonDS::s32 ADPCMVal;
    melonDS::s32 ADPCMIndex;
    melonDS::s32 ADPCMValLoop;
    melonDS::s32 ADPCMIndexLoop;
    int Peak;
};

struct SoundCaptureState
{
    melonDS::u8 Cnt;
    melonDS::u32 DstAddr;
    melonDS::u16 TimerReload;
    melonDS::u32 Length;
    melonDS::s32 Pos;
};

struct SoundState
{
    bool Valid;
    melonDS::u32 SectionLength;
    melonDS::u16 Cnt;
    melonDS::u16 Bias;
    bool Mute;
    melonDS::u16 DebugMute;
    bool CaptureMuted;
    SoundChannelState Channels[16];
    SoundCaptureState Captures[2];
};

class SoundWindow : public DebugTool
{
    Q_OBJECT

public:
    SoundWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QLabel* Master;
    QTableWidget* Channels;
    QTableWidget* Captures;
    QProgressBar* Levels[16];
    QLabel* Info;
    QCheckBox* MuteBoxes[16];
    QCheckBox* CaptureMuted;
    bool ColumnsSized = false;

    void showState(const SoundState& state);
    void setMute(melonDS::u16 mask, int captureMuted);
};

#endif
