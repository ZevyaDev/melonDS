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
#include <cstdlib>
#include <cstring>
#include <vector>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include "SoundWindow.h"
#include "DebugSymbols.h"
#include "Savestate.h"
#include "SPU.h"

using namespace melonDS;

constexpr double SoundTimerClock = 33513982.0 / 2;
constexpr int PeakWindow = 512;
constexpr int LevelScale = 1000;
constexpr u32 StateHeaderSize = 16;
constexpr u32 SectionHeaderSize = 16;
constexpr u32 MasterStateSize = 17;
constexpr u32 ChannelStateSize = 105;
constexpr u32 CaptureStateSize = 51;
constexpr u32 SPUSectionSize = SectionHeaderSize + MasterStateSize + 16 * ChannelStateSize + 2 * CaptureStateSize;

enum ChannelColumn
{
    chan_Mute,
    chan_On,
    chan_Format,
    chan_Volume,
    chan_Pan,
    chan_Rate,
    chan_Source,
    chan_LoopStart,
    chan_Length,
    chan_Position,
    chan_Repeat,
    chan_Hold,
    chan_Level,
};

enum CaptureColumn
{
    cap_Active,
    cap_Source,
    cap_Add,
    cap_Repeat,
    cap_Format,
    cap_Destination,
    cap_Length,
    cap_Position,
    cap_Rate,
};

static const char* const RepeatNames[4] = {"manual", "loop", "one-shot", "invalid"};
static const int VolumeDividers[4] = {1, 2, 4, 16};
static const char* const OutputNames[2][4] = {{"left mixer", "ch1", "ch3", "ch1+ch3"}, {"right mixer", "ch1", "ch3", "ch1+ch3"}};

class StateReader
{
public:
    StateReader(const u8* data, u32 size) : Data(data), Size(size) {}

    template <typename T>
    T get()
    {
        T value {};
        if (Offset + sizeof(T) <= Size)
            memcpy(&value, Data + Offset, sizeof(T));
        Offset += sizeof(T);
        return value;
    }

    void skip(u32 bytes) { Offset += bytes; }

private:
    const u8* Data;
    u32 Size;
    u32 Offset = 0;
};

static u32 ChannelFormat(u32 cnt)
{
    return (cnt >> 29) & 0x3;
}

static bool ChannelLoops(u32 cnt)
{
    return ((cnt >> 27) & 0x3) == 1;
}

static int PCMPeak(NDS& nds, const SoundChannelState& ch, u32 end)
{
    u32 step = ChannelFormat(ch.Cnt) == 1 ? 2 : 1;
    u32 offset = ch.Pos > 0 ? (u32)ch.Pos * step : 0;
    int peak = std::abs(ch.CurSample);
    for (int i = 0; i < PeakWindow; i++)
    {
        if (offset >= end)
        {
            if (!ChannelLoops(ch.Cnt) || !ch.Length)
                break;
            offset = ch.LoopPos + (offset - end) % ch.Length;
        }
        u32 addr = ch.SrcAddr + offset;
        int sample = step == 2 ? (s16)nds.Debugger.Peek(1, addr, 2) : (s8)nds.Debugger.Peek(1, addr, 1) * 256;
        peak = std::max(peak, std::abs(sample));
        offset += step;
    }
    return peak;
}

static int ADPCMPeak(NDS& nds, const SoundChannelState& ch, u32 end)
{
    s32 value = ch.ADPCMVal;
    s32 index = ch.ADPCMIndex;
    if (ch.Pos < 0)
    {
        u32 header = nds.Debugger.Peek(1, ch.SrcAddr, 4);
        value = (s16)(header & 0xFFFF);
        index = (header >> 16) & 0x7F;
    }
    index = std::clamp(index, 0, 88);
    s32 nibble = std::max(ch.Pos + 1, 8);
    int peak = std::abs(ch.CurSample);
    for (int i = 0; i < PeakWindow; i++)
    {
        if ((u32)(nibble >> 1) >= end)
        {
            if (!ChannelLoops(ch.Cnt) || !ch.Length)
                break;
            nibble = (ch.LoopPos << 1) + 1;
            value = ch.ADPCMValLoop;
            index = std::clamp(ch.ADPCMIndexLoop, 0, 88);
            continue;
        }
        u8 data = nds.Debugger.Peek(1, ch.SrcAddr + (nibble >> 1), 1);
        u8 code = (nibble & 1) ? data >> 4 : data & 0xF;
        u16 step = SPUChannel::ADPCMTable[index];
        s32 diff = step >> 3;
        if (code & 0x1)
            diff += step >> 2;
        if (code & 0x2)
            diff += step >> 1;
        if (code & 0x4)
            diff += step;
        value = (code & 0x8) ? std::max(value - diff, -0x7FFF) : std::min(value + diff, 0x7FFF);
        index = std::clamp(index + SPUChannel::ADPCMIndexTable[code & 0x7], 0, 88);
        peak = std::max(peak, std::abs(value));
        nibble++;
    }
    return peak;
}

static int ChannelPeak(NDS& nds, int num, const SoundChannelState& ch)
{
    if (!(ch.Cnt & (1u << 31)))
        return 0;
    u32 format = ChannelFormat(ch.Cnt);
    if (format == 3)
    {
        if (num >= 14)
            return 0x7FFF;
        if (num >= 8)
            return ((ch.Cnt >> 24) & 0x7) == 7 ? 0 : 0x7FFF;
        return 0;
    }
    u32 end = ch.LoopPos + ch.Length;
    if (end < 16)
        return 0;
    return format == 2 ? ADPCMPeak(nds, ch, end) : PCMPeak(nds, ch, end);
}

static SoundState ReadSPUThroughSavestate(NDS& nds)
{
    SoundState state {};
    std::vector<u8> buffer(4096);
    Savestate file(buffer.data(), buffer.size(), true);
    nds.SPU.DoSavestate(&file);
    file.Finish();
    if (file.Error || memcmp(&buffer[StateHeaderSize], "SPU.", 4) != 0)
        return state;
    memcpy(&state.SectionLength, &buffer[StateHeaderSize + 4], 4);
    if (state.SectionLength != SPUSectionSize)
        return state;

    StateReader in(&buffer[StateHeaderSize + SectionHeaderSize], SPUSectionSize - SectionHeaderSize);
    state.Cnt = in.get<u16>();
    in.skip(sizeof(u8));
    state.Bias = in.get<u16>();
    in.skip(2 * sizeof(s16) + sizeof(u32));
    state.Mute = in.get<u32>() != 0;
    for (SoundChannelState& ch : state.Channels)
    {
        ch.Cnt = in.get<u32>();
        ch.SrcAddr = in.get<u32>();
        ch.TimerReload = in.get<u16>();
        ch.LoopPos = in.get<u32>();
        ch.Length = in.get<u32>();
        ch.Volume = in.get<u8>();
        ch.VolumeShift = in.get<u8>();
        ch.Pan = in.get<u8>();
        in.skip(sizeof(u8) + sizeof(u32));
        ch.Pos = in.get<s32>();
        in.skip(3 * sizeof(s16));
        ch.CurSample = in.get<s16>();
        in.skip(sizeof(u16));
        ch.ADPCMVal = in.get<s32>();
        ch.ADPCMIndex = in.get<s32>();
        ch.ADPCMValLoop = in.get<s32>();
        ch.ADPCMIndexLoop = in.get<s32>();
        in.skip(sizeof(u8) + 4 * sizeof(u32) + 8 * sizeof(u32));
    }
    for (SoundCaptureState& cap : state.Captures)
    {
        cap.Cnt = in.get<u8>();
        cap.DstAddr = in.get<u32>();
        cap.TimerReload = in.get<u16>();
        cap.Length = in.get<u32>();
        in.skip(sizeof(u32));
        cap.Pos = in.get<s32>();
        in.skip(4 * sizeof(u32) + 4 * sizeof(u32));
    }
    state.Valid = true;
    for (int i = 0; i < 16; i++)
        state.Channels[i].Peak = ChannelPeak(nds, i, state.Channels[i]);
    return state;
}

static double SampleRate(u16 reload)
{
    return SoundTimerClock / (0x10000 - reload);
}

static QString RateText(u16 reload)
{
    return QString("%1 Hz").arg(SampleRate(reload), 0, 'f', 0);
}

static QString FormatText(int num, u32 cnt)
{
    switch (ChannelFormat(cnt))
    {
    case 0:
        return "PCM8";
    case 1:
        return "PCM16";
    case 2:
        return "ADPCM";
    }
    if (num >= 14)
        return "noise";
    if (num >= 8)
    {
        u32 duty = (cnt >> 24) & 0x7;
        return QString("PSG %1%").arg(duty == 7 ? 0.0 : (duty + 1) * 12.5);
    }
    return "PSG (none)";
}

static QString PanText(u32 cnt)
{
    int pan = (cnt >> 16) & 0x7F;
    return QString::number(pan) + (pan < 64 ? " L" : pan > 64 ? " R" : " C");
}

static QString PositionText(const SoundChannelState& ch)
{
    u32 format = ChannelFormat(ch.Cnt);
    if (format == 3)
        return "-";
    if (ch.Pos < 0)
        return "start";
    u32 offset = format == 0 ? ch.Pos : format == 1 ? ch.Pos * 2 : ch.Pos / 2;
    u32 end = ch.LoopPos + ch.Length;
    return Hex(offset, 6) + (end ? QString(" %1%").arg(qMin<u64>(offset * 100ull / end, 999)) : QString());
}

static int LevelOf(const SoundChannelState& ch)
{
    s64 output = ((s64)ch.Peak * ch.Volume) << ch.VolumeShift;
    s64 full = ((s64)0x7FFF * 128) << 4;
    return (int)std::min<s64>(output * LevelScale / full, LevelScale);
}

static QString AddressText(DebugSymbols& syms, u32 addr)
{
    QString name = syms.describe(addr);
    return Hex(addr) + (name.isEmpty() ? "" : "  " + name);
}

static QString MasterText(const SoundState& s)
{
    u16 cnt = s.Cnt;
    return QString("SOUNDCNT %1: %2, volume %3, left output %4, right output %5, ch1 %6, ch3 %7 | SOUNDBIAS %8 | speakers %9")
        .arg(Hex(cnt, 4)).arg((cnt & 0x8000) ? "enabled" : "disabled").arg(cnt & 0x7F)
        .arg(OutputNames[0][(cnt >> 8) & 0x3]).arg(OutputNames[1][(cnt >> 10) & 0x3])
        .arg((cnt & (1 << 12)) ? "not mixed" : "mixed").arg((cnt & (1 << 13)) ? "not mixed" : "mixed")
        .arg(Hex(s.Bias, 3)).arg(s.Mute ? "off (POWCNT2)" : "on");
}

static void SetCell(QTableWidget* table, int row, int column, const QString& text)
{
    QTableWidgetItem* item = table->item(row, column);
    if (!item)
        table->setItem(row, column, new QTableWidgetItem(text));
    else if (item->text() != text)
        item->setText(text);
}

static QTableWidget* MakeTable(QWidget* parent, int rows, const QStringList& headers)
{
    QTableWidget* table = new QTableWidget(rows, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    QStringList labels;
    for (int i = 0; i < rows; i++)
        labels << QString::number(i);
    table->setVerticalHeaderLabels(labels);
    table->setFont(MonoFont());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 4);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

SoundWindow::SoundWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Sound", parent)
{
    resize(1150, 600);
    QVBoxLayout* layout = new QVBoxLayout(this);
    Master = new QLabel(this);
    Master->setFont(MonoFont());
    Master->setWordWrap(true);
    Master->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(Master);

    QHBoxLayout* mutes = new QHBoxLayout();
    QPushButton* solo = new QPushButton("Solo selected", this);
    solo->setObjectName("solo");
    QPushButton* unmute = new QPushButton("Unmute all", this);
    unmute->setObjectName("unmuteAll");
    QPushButton* analyze = new QPushButton("Analyze capture", this);
    analyze->setObjectName("analyzeCapture");
    analyze->setToolTip("Mute everything but channels 1 and 3, keep muted channels in the capture units");
    CaptureMuted = new QCheckBox("Capture muted channels", this);
    CaptureMuted->setObjectName("captureMuted");
    CaptureMuted->setToolTip("Muted channels still reach the capture units, so the game hears them; only the speakers do not");
    mutes->addWidget(solo);
    mutes->addWidget(unmute);
    mutes->addWidget(analyze);
    mutes->addWidget(CaptureMuted);
    mutes->addStretch();
    layout->addLayout(mutes);

    Channels = MakeTable(this, 16, {"Mute", "On", "Format", "Volume", "Pan", "Rate", "Source", "Loop start", "Length", "Position", "Repeat", "Hold", "Level"});
    for (int i = 0; i < 16; i++)
    {
        MuteBoxes[i] = new QCheckBox(Channels);
        MuteBoxes[i]->setObjectName(QString("mute%1").arg(i));
        connect(MuteBoxes[i], &QCheckBox::toggled, this, [this, i](bool on) {
            Bridge->post([i, on](NDS& nds) {
                if (on)
                    nds.SPU.DebugMute |= 1 << i;
                else
                    nds.SPU.DebugMute &= ~(1 << i);
            });
        });
        Channels->setCellWidget(i, chan_Mute, MuteBoxes[i]);
        Levels[i] = new QProgressBar(Channels);
        Levels[i]->setRange(0, LevelScale);
        Levels[i]->setTextVisible(false);
        Levels[i]->setMaximumHeight(Channels->verticalHeader()->defaultSectionSize() - 4);
        Channels->setCellWidget(i, chan_Level, Levels[i]);
    }
    layout->addWidget(Channels, 1);

    layout->addWidget(new QLabel("Capture units", this));
    Captures = MakeTable(this, 2, {"Active", "Source", "Add", "Repeat", "Format", "Destination", "Length", "Position", "Rate"});
    Captures->setMaximumHeight(Captures->verticalHeader()->defaultSectionSize() * 2 + Captures->horizontalHeader()->sizeHint().height() + 2 * Captures->frameWidth() + 4);
    layout->addWidget(Captures);

    Info = new QLabel("Level: peak of the next 512 samples, scaled by channel volume and divider.", this);
    layout->addWidget(Info);

    connect(solo, &QPushButton::clicked, this, [this] {
        u16 keep = 0;
        for (const QModelIndex& index : Channels->selectionModel()->selectedRows())
            keep |= 1 << index.row();
        if (!keep && Channels->currentRow() >= 0)
            keep = 1 << Channels->currentRow();
        setMute(~keep & 0xFFFF, -1);
    });
    connect(unmute, &QPushButton::clicked, this, [this] { setMute(0, -1); });
    connect(analyze, &QPushButton::clicked, this, [this] { setMute(0xFFFF & ~((1 << 1) | (1 << 3)), 1); });
    connect(CaptureMuted, &QCheckBox::toggled, this, [this](bool on) {
        Bridge->post([on](NDS& nds) { nds.SPU.DebugCaptureMuted = on; });
    });
    setInterval(100);
}

void SoundWindow::setMute(u16 mask, int captureMuted)
{
    Bridge->post([mask, captureMuted](NDS& nds) {
        nds.SPU.DebugMute = mask;
        if (captureMuted >= 0)
            nds.SPU.DebugCaptureMuted = captureMuted;
    });
    requestRefresh();
}

void SoundWindow::refresh()
{
    fetch([](NDS& nds) {
        SoundState state = ReadSPUThroughSavestate(nds);
        state.DebugMute = nds.SPU.DebugMute;
        state.CaptureMuted = nds.SPU.DebugCaptureMuted;
        return state;
    }, [this](const SoundState& state) { showState(state); });
}

void SoundWindow::showState(const SoundState& state)
{
    if (!state.Valid)
    {
        Info->setText(QString("SPU state layout not recognised (section length %1, expected %2)").arg(state.SectionLength).arg(SPUSectionSize));
        return;
    }
    DebugSymbols& syms = Bridge->symbols();
    Master->setText(MasterText(state));
    for (int i = 0; i < 16; i++)
    {
        QSignalBlocker block(MuteBoxes[i]);
        MuteBoxes[i]->setChecked(state.DebugMute & (1 << i));
    }
    {
        QSignalBlocker block(CaptureMuted);
        CaptureMuted->setChecked(state.CaptureMuted);
    }
    for (int i = 0; i < 16; i++)
    {
        const SoundChannelState& ch = state.Channels[i];
        u32 cnt = ch.Cnt;
        bool generated = ChannelFormat(cnt) == 3;
        QString rate = RateText(ch.TimerReload);
        if (generated && i >= 8 && i < 14)
            rate += QString(" (tone %1 Hz)").arg(SampleRate(ch.TimerReload) / 8, 0, 'f', 0);
        SetCell(Channels, i, chan_On, (cnt & (1u << 31)) ? "on" : "off");
        SetCell(Channels, i, chan_Format, FormatText(i, cnt));
        SetCell(Channels, i, chan_Volume, QString("%1 /%2").arg(cnt & 0x7F).arg(VolumeDividers[(cnt >> 8) & 0x3]));
        SetCell(Channels, i, chan_Pan, PanText(cnt));
        SetCell(Channels, i, chan_Rate, rate);
        SetCell(Channels, i, chan_Source, generated ? QString("-") : AddressText(syms, ch.SrcAddr));
        SetCell(Channels, i, chan_LoopStart, generated ? QString("-") : Hex(ch.LoopPos, 5));
        SetCell(Channels, i, chan_Length, generated ? QString("-") : Hex(ch.Length, 6));
        SetCell(Channels, i, chan_Position, PositionText(ch));
        SetCell(Channels, i, chan_Repeat, generated ? QString("-") : RepeatNames[(cnt >> 27) & 0x3]);
        SetCell(Channels, i, chan_Hold, (cnt & (1u << 15)) ? "hold" : "");
        int level = LevelOf(ch);
        Levels[i]->setValue(std::max(level, Levels[i]->value() * 3 / 4));
    }
    for (int i = 0; i < 2; i++)
    {
        const SoundCaptureState& cap = state.Captures[i];
        u32 percent = cap.Length ? qMin<u64>((u64)std::max(cap.Pos, 0) * 100 / cap.Length, 999) : 0;
        SetCell(Captures, i, cap_Active, (cap.Cnt & 0x80) ? "on" : "off");
        SetCell(Captures, i, cap_Source, (cap.Cnt & 0x02) ? (i ? "channel 2" : "channel 0") : (i ? "right mixer" : "left mixer"));
        SetCell(Captures, i, cap_Add, (cap.Cnt & 0x01) ? (i ? "ch3 into ch2" : "ch1 into ch0") : "-");
        SetCell(Captures, i, cap_Repeat, (cap.Cnt & 0x04) ? "one-shot" : "loop");
        SetCell(Captures, i, cap_Format, (cap.Cnt & 0x08) ? "PCM8" : "PCM16");
        SetCell(Captures, i, cap_Destination, AddressText(syms, cap.DstAddr));
        SetCell(Captures, i, cap_Length, Hex(cap.Length, 5));
        SetCell(Captures, i, cap_Position, QString("%1 %2%").arg(Hex(cap.Pos, 5)).arg(percent));
        SetCell(Captures, i, cap_Rate, RateText(cap.TimerReload));
    }
    if (!ColumnsSized)
    {
        Channels->resizeColumnsToContents();
        Captures->resizeColumnsToContents();
        ColumnsSized = true;
    }
}
