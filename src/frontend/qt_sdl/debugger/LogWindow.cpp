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
#include <deque>
#include <mutex>
#include <vector>

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include "LogWindow.h"
#include "EmuInstance.h"

namespace DebugLog
{

struct Entry
{
    unsigned long long Seq;
    int Instance;
    int Source;
    QString Text;
};

constexpr size_t MaxEntries = 20000;
constexpr size_t MaxPartial = 4096;
constexpr int MaxInstances = 16;

static std::mutex Lock;
static std::deque<Entry> Entries;
static unsigned long long NextSeq = 1;
static std::string Partial[MaxInstances + 1][3];

static void Push(int instance, int source, const std::string& line)
{
    Entries.push_back({NextSeq++, instance, source, QString::fromUtf8(line.data(), (int)line.size())});
    if (Entries.size() > MaxEntries)
        Entries.pop_front();
}

void append(int instance, int source, const char* text, bool newline)
{
    if (source < 0 || source > source_ARM7)
        return;
    std::lock_guard lock(Lock);
    int slot = instance < 0 || instance >= MaxInstances ? MaxInstances : instance;
    std::string& partial = Partial[slot][source];
    partial += text;
    if (newline)
        partial += '\n';
    size_t pos;
    while ((pos = partial.find('\n')) != std::string::npos)
    {
        Push(instance, source, partial.substr(0, pos));
        partial.erase(0, pos + 1);
    }
    if (partial.size() > MaxPartial)
    {
        Push(instance, source, partial);
        partial.clear();
    }
}

static std::vector<Entry> Since(unsigned long long seq)
{
    std::lock_guard lock(Lock);
    std::vector<Entry> out;
    for (auto it = Entries.rbegin(); it != Entries.rend() && it->Seq > seq; ++it)
        out.push_back(*it);
    std::reverse(out.begin(), out.end());
    return out;
}

static void Clear(int instance)
{
    std::lock_guard lock(Lock);
    Entries.erase(std::remove_if(Entries.begin(), Entries.end(), [instance](const Entry& e) {
        return e.Instance == instance || e.Instance < 0;
    }), Entries.end());
}

}

static const char* const SourceTags[3] = {"[emu] ", "[ARM9] ", "[ARM7] "};

LogWindow::LogWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Log", parent)
{
    resize(820, 480);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* bar = new QHBoxLayout();
    ShowARM9 = new QCheckBox("ARM9 prints", this);
    ShowARM9->setObjectName("showARM9");
    ShowARM9->setChecked(true);
    ShowARM7 = new QCheckBox("ARM7 prints", this);
    ShowARM7->setObjectName("showARM7");
    ShowARM7->setChecked(true);
    ShowEmulator = new QCheckBox("Emulator log", this);
    ShowEmulator->setObjectName("showEmulator");
    Frozen = new QCheckBox("Freeze", this);
    Frozen->setObjectName("freeze");
    QPushButton* clear = new QPushButton("Clear", this);
    clear->setObjectName("clear");
    QPushButton* save = new QPushButton("Save...", this);
    save->setObjectName("save");
    bar->addWidget(ShowARM9);
    bar->addWidget(ShowARM7);
    bar->addWidget(ShowEmulator);
    bar->addWidget(Frozen);
    bar->addStretch();
    bar->addWidget(clear);
    bar->addWidget(save);
    layout->addLayout(bar);

    View = new QPlainTextEdit(this);
    View->setObjectName("log");
    View->setReadOnly(true);
    View->setFont(MonoFont());
    View->setMaximumBlockCount((int)DebugLog::MaxEntries);
    View->setLineWrapMode(QPlainTextEdit::NoWrap);
    View->setPlaceholderText("Game output through the no$gba debug registers (0x04FFFA10/14/18/1C) and MOV R12,R12 messages appears here.");
    layout->addWidget(View, 1);

    for (QCheckBox* box : {ShowARM9, ShowARM7, ShowEmulator})
        connect(box, &QCheckBox::toggled, this, &LogWindow::reload);
    connect(Frozen, &QCheckBox::toggled, this, [this](bool on) {
        if (!on)
            refresh();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        DebugLog::Clear(Bridge->instance()->getInstanceID());
        View->clear();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getSaveFileName(this, "Save log", QString(), "Text files (*.txt)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(View->toPlainText().toUtf8());
    });
    setInterval(100);
}

void LogWindow::reload()
{
    View->clear();
    Seen = 0;
    refresh();
}

void LogWindow::refresh()
{
    if (Frozen->isChecked())
        return;
    int instance = Bridge->instance()->getInstanceID();
    std::vector<DebugLog::Entry> entries = DebugLog::Since(Seen);
    if (entries.empty())
        return;
    QScrollBar* bar = View->verticalScrollBar();
    bool atEnd = bar->value() >= bar->maximum() - 2;
    QStringList lines;
    for (const DebugLog::Entry& e : entries)
    {
        Seen = e.Seq;
        if (e.Instance >= 0 && e.Instance != instance)
            continue;
        if ((e.Source == DebugLog::source_ARM9 && !ShowARM9->isChecked())
            || (e.Source == DebugLog::source_ARM7 && !ShowARM7->isChecked())
            || (e.Source == DebugLog::source_Emulator && !ShowEmulator->isChecked()))
            continue;
        lines << SourceTags[e.Source] + e.Text;
    }
    if (lines.isEmpty())
        return;
    View->appendPlainText(lines.join('\n'));
    if (atEnd)
        bar->setValue(bar->maximum());
}
