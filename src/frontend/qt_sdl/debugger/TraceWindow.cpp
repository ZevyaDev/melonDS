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
#include <map>
#include <set>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "TraceWindow.h"
#include "DebugSymbolsQt.h"
#include "DebugTools.h"
#include "Disassembler.h"

using namespace melonDS;

constexpr int MaxShownTrace = 20000;

static QComboBox* CPUCombo(QWidget* parent, bool both)
{
    QComboBox* box = new QComboBox(parent);
    box->addItems({"ARM9", "ARM7"});
    if (both)
        box->addItem("both");
    return box;
}

TraceWindow::TraceWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Trace", parent)
{
    resize(900, 600);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    CPU = CPUCombo(this, true);
    Start = new QLineEdit("02000000", this);
    End = new QLineEdit("FFFFFFFF", this);
    Capacity = new QSpinBox(this);
    Capacity->setRange(1000, 10000000);
    Capacity->setSingleStep(10000);
    Capacity->setValue(200000);
    Active = new QCheckBox("Record", this);
    QPushButton* fetch = new QPushButton("Fetch", this);
    QPushButton* save = new QPushButton("Save...", this);
    row->addWidget(new QLabel("CPU", this));
    row->addWidget(CPU);
    row->addWidget(new QLabel("PC from", this));
    row->addWidget(Start);
    row->addWidget(new QLabel("to", this));
    row->addWidget(End);
    row->addWidget(new QLabel("ring size", this));
    row->addWidget(Capacity);
    row->addWidget(Active);
    row->addWidget(fetch);
    row->addWidget(save);
    layout->addLayout(row);

    Table = new QTableWidget(0, 5, this);
    Table->setHorizontalHeaderLabels({"Cycle", "CPU", "PC", "Function", "Instruction"});
    Table->setFont(MonoFont());
    Table->verticalHeader()->hide();
    Table->horizontalHeader()->setStretchLastSection(true);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    Table->verticalHeader()->setDefaultSectionSize(Table->fontMetrics().height() + 2);
    layout->addWidget(Table);
    Info = new QLabel("Record fills a ring buffer with every executed instruction in the range; Fetch drains it.", this);
    layout->addWidget(Info);

    connect(Active, &QCheckBox::toggled, this, &TraceWindow::apply);
    connect(fetch, &QPushButton::clicked, this, &TraceWindow::drain);
    connect(save, &QPushButton::clicked, this, &TraceWindow::save);
    connect(Table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        DebugTools::showDisassembly(Bridge, parentWidget(), Table->item(row, 2)->text().toUInt(nullptr, 16));
    });
    setLiveRefresh(false);
}

void TraceWindow::apply()
{
    u32 start = 0, end = 0xFFFFFFFF;
    Bridge->symbols().parseAddress(SymStr(Start->text()), start);
    Bridge->symbols().parseAddress(SymStr(End->text()), end);
    u8 mask = CPU->currentIndex() == 2 ? 3 : 1 << CPU->currentIndex();
    u32 cap = Active->isChecked() ? Capacity->value() : 0;
    Bridge->post([mask, start, end, cap](NDS& nds) { nds.Debugger.SetTrace(mask, start, end, cap); });
    Info->setText(cap ? QString("Recording %1-%2").arg(Hex(start)).arg(Hex(end)) : "Stopped");
}

void TraceWindow::drain()
{
    Bridge->call(this, [](NDS& nds) { return nds.Debugger.DrainTrace(); },
        [this](const std::vector<TraceEntry>& entries) {
            Entries = entries;
            showEntries(entries);
        });
}

static QString Decode(Disassembler& dis, DebugSymbols& syms, const TraceEntry& e)
{
    bool thumb = e.CPSR & 0x20;
    u8 bytes[4];
    memcpy(bytes, &e.Instr, 4);
    DisasmLine line = dis.decode(e.PC, bytes, thumb ? 2 : 4, thumb);
    if (line.HasTarget && !line.IsLiteral)
    {
        QString name = SymStr(syms.describe(line.Target, false));
        if (!name.isEmpty())
            return SymStr(line.Mnemonic) + " " + name;
    }
    return SymStr(line.Mnemonic + " " + line.Operands);
}

void TraceWindow::showEntries(const std::vector<TraceEntry>& entries)
{
    Disassembler dis;
    DebugSymbols& syms = Bridge->symbols();
    size_t first = entries.size() > MaxShownTrace ? entries.size() - MaxShownTrace : 0;
    Table->setRowCount(0);
    Table->setRowCount(entries.size() - first);
    for (size_t i = first; i < entries.size(); i++)
    {
        const TraceEntry& e = entries[i];
        int r = i - first;
        Table->setItem(r, 0, new QTableWidgetItem(QString::number(e.Timestamp)));
        Table->setItem(r, 1, new QTableWidgetItem(e.CPU ? "7" : "9"));
        Table->setItem(r, 2, new QTableWidgetItem(Hex(e.PC)));
        Table->setItem(r, 3, new QTableWidgetItem(SymStr(syms.describe(e.PC))));
        Table->setItem(r, 4, new QTableWidgetItem(Decode(dis, syms, e)));
    }
    Table->resizeColumnsToContents();
    Info->setText(first ? QString("Showing the last %1 of %2 entries; Save writes all of them").arg(MaxShownTrace).arg(entries.size())
                        : QString("%1 entries").arg(entries.size()));
}

void TraceWindow::save()
{
    if (Entries.empty())
        return;
    QString path = QFileDialog::getSaveFileName(this, "Save trace", "trace.txt", "Text (*.txt)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "melonDS", "Cannot write " + path);
        return;
    }
    QTextStream out(&f);
    Disassembler dis;
    DebugSymbols& syms = Bridge->symbols();
    for (const TraceEntry& e : Entries)
        out << e.Timestamp << ' ' << (e.CPU ? 7 : 9) << ' ' << Hex(e.PC) << ' ' << Decode(dis, syms, e) << "  ; " << SymStr(syms.describe(e.PC)) << '\n';
}

ProfilerWindow::ProfilerWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Call counts and coverage", parent)
{
    resize(760, 600);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    CPU = CPUCombo(this, false);
    Calls = new QCheckBox("Count calls", this);
    Cover = new QCheckBox("Record coverage", this);
    QPushButton* reset = new QPushButton("Reset", this);
    QPushButton* save = new QPushButton("Save coverage...", this);
    row->addWidget(new QLabel("CPU", this));
    row->addWidget(CPU);
    row->addWidget(Calls);
    row->addWidget(Cover);
    row->addStretch();
    row->addWidget(reset);
    row->addWidget(save);
    layout->addLayout(row);

    QTabWidget* tabs = new QTabWidget(this);
    CallTree = new QTreeWidget(this);
    CallTree->setHeaderLabels({"Function", "Entry", "Calls"});
    CoverTree = new QTreeWidget(this);
    CoverTree->setHeaderLabels({"Function", "Start", "Instructions hit", "Size", "Covered %"});
    for (QTreeWidget* t : {CallTree, CoverTree})
    {
        t->setRootIsDecorated(false);
        t->setFont(MonoFont());
        t->setSortingEnabled(true);
        connect(t, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
            DebugTools::showDisassembly(Bridge, parentWidget(), item->text(1).toUInt(nullptr, 16));
        });
    }
    tabs->addTab(CallTree, "Call counts");
    tabs->addTab(CoverTree, "Coverage");
    layout->addWidget(tabs);
    Info = new QLabel("Counts calls made with BL/BLX and records every executed instruction address. Updates live.", this);
    layout->addWidget(Info);

    connect(Calls, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            Bridge->retainCallTracking();
        else
            Bridge->releaseCallTracking();
    });
    connect(Cover, &QCheckBox::toggled, this, [this](bool on) {
        Bridge->post([on](NDS& nds) { nds.Debugger.SetCoverage(on); });
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        int cpu = CPU->currentIndex();
        Bridge->post([cpu](NDS& nds) {
            nds.Debugger.DrainCallCounts(cpu);
            nds.Debugger.DrainCoverage(cpu);
        });
        CallTree->clear();
        CoverTree->clear();
        CoverAddrs.clear();
    });
    connect(save, &QPushButton::clicked, this, &ProfilerWindow::saveCoverage);
    setInterval(1000);
}

void ProfilerWindow::refresh()
{
    if (Calls->isChecked())
        drainCalls();
    if (Cover->isChecked())
        drainCoverage();
}

ProfilerWindow::~ProfilerWindow()
{
    if (Calls->isChecked())
        Bridge->releaseCallTracking();
    if (Cover->isChecked())
        Bridge->post([](NDS& nds) { nds.Debugger.SetCoverage(false); });
}

void ProfilerWindow::drainCalls()
{
    int cpu = CPU->currentIndex();
    Bridge->call(this, [cpu](NDS& nds) { return nds.Debugger.DrainCallCounts(cpu); },
        [this](const std::unordered_map<u32, u64>& counts) {
            if (counts.empty())
                return;
            QHash<u32, QTreeWidgetItem*> rows;
            for (int i = 0; i < CallTree->topLevelItemCount(); i++)
            {
                QTreeWidgetItem* item = CallTree->topLevelItem(i);
                rows.insert(item->text(1).toUInt(nullptr, 16), item);
            }
            CallTree->setSortingEnabled(false);
            DebugSymbols& syms = Bridge->symbols();
            for (auto& [entry, n] : counts)
            {
                QTreeWidgetItem* item = rows.value(entry);
                qulonglong total = n + (item ? item->data(2, Qt::UserRole).toULongLong() : 0);
                if (!item)
                    item = new QTreeWidgetItem(CallTree, {SymStr(syms.describe(entry)), Hex(entry), ""});
                item->setData(2, Qt::UserRole, total);
                item->setData(2, Qt::DisplayRole, total);
            }
            CallTree->setSortingEnabled(true);
            Info->setText(QString("%1 functions called").arg(CallTree->topLevelItemCount()));
        });
}

void ProfilerWindow::drainCoverage()
{
    int cpu = CPU->currentIndex();
    Bridge->call(this, [cpu](NDS& nds) { return nds.Debugger.DrainCoverage(cpu); },
        [this](const std::vector<u32>& addrs) {
            if (addrs.empty())
                return;
            std::vector<u32> merged;
            std::set_union(CoverAddrs.begin(), CoverAddrs.end(), addrs.begin(), addrs.end(), std::back_inserter(merged));
            CoverAddrs.swap(merged);
            DebugSymbols& syms = Bridge->symbols();
            struct Func
            {
                QString Name;
                u32 Hits = 0;
                u32 Size = 0;
                u32 InstrSize = 4;
            };
            std::map<u32, Func> funcs;
            for (u32 a : CoverAddrs)
            {
                const DebugSymbols::Symbol* s = syms.lookup(a);
                u32 start = s ? s->Addr : (a & ~0xFFu);
                Func& f = funcs[start];
                if (f.Name.isEmpty())
                    f = {s ? SymStr(syms.name(*s)) : QString("unknown %1").arg(Hex(start)), 0, s ? s->Size : 0, s && s->Thumb ? 2u : 4u};
                f.Hits++;
            }
            CoverTree->setSortingEnabled(false);
            CoverTree->clear();
            for (auto& [start, f] : funcs)
            {
                auto* item = new QTreeWidgetItem(CoverTree, {f.Name, Hex(start), "", f.Size ? "0x" + QString::number(f.Size, 16) : ""});
                item->setData(2, Qt::DisplayRole, f.Hits);
                if (f.Size)
                    item->setData(4, Qt::DisplayRole, std::min(100, (int)(f.Hits * f.InstrSize * 100 / f.Size)));
            }
            CoverTree->setSortingEnabled(true);
            Info->setText(QString("%1 addresses in %2 functions").arg(CoverAddrs.size()).arg(funcs.size()));
        });
}

void ProfilerWindow::saveCoverage()
{
    QString path = QFileDialog::getSaveFileName(this, "Save coverage", "coverage.txt", "Text (*.txt)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&f);
    for (u32 a : CoverAddrs)
        out << Hex(a) << '\n';
}

LastWriterWindow::LastWriterWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Last writer", parent)
{
    resize(760, 500);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    Enabled = new QCheckBox("Track writes to main RAM", this);
    Addr = new QLineEdit("02000000", this);
    Addr->setObjectName("address");
    Length = new QSpinBox(this);
    Length->setRange(4, 0x1000);
    Length->setSingleStep(4);
    Length->setValue(0x40);
    Length->setDisplayIntegerBase(16);
    Length->setPrefix("0x");
    row->addWidget(Enabled);
    row->addWidget(new QLabel("Address", this));
    row->addWidget(Addr);
    row->addWidget(new QLabel("Bytes", this));
    row->addWidget(Length);
    layout->addLayout(row);
    Tree = new QTreeWidget(this);
    Tree->setHeaderLabels({"Address", "Value", "Written by", "Function"});
    Tree->setRootIsDecorated(false);
    Tree->setFont(MonoFont());
    layout->addWidget(Tree);
    Info = new QLabel("Records the instruction (or DMA channel) behind the latest write to each word of main RAM.", this);
    layout->addWidget(Info);

    connect(Enabled, &QCheckBox::toggled, this, [this](bool on) {
        Bridge->post([on](NDS& nds) { nds.Debugger.SetLastWriter(on); });
        requestRefresh();
    });
    connect(Addr, &QLineEdit::editingFinished, this, &LastWriterWindow::requestRefresh);
    connect(Length, &QSpinBox::valueChanged, this, &LastWriterWindow::requestRefresh);
    connect(Tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        bool ok;
        u32 pc = item->text(2).toUInt(&ok, 16);
        if (ok)
            DebugTools::showDisassembly(Bridge, parentWidget(), pc);
    });
    setInterval(500);
}

void LastWriterWindow::refresh()
{
    u32 start;
    if (!Bridge->symbols().parseAddress(SymStr(Addr->text()), start))
        return;
    start &= ~3u;
    u32 len = Length->value() & ~3u;
    fetch([start, len](NDS& nds) {
        std::vector<std::pair<u32, u32>> out;
        for (u32 a = start; a < start + len; a += 4)
            out.emplace_back(nds.Debugger.Peek(0, a, 4), nds.Debugger.GetLastWriter(a));
        return out;
    }, [this, start](const std::vector<std::pair<u32, u32>>& rows) {
        DebugSymbols& syms = Bridge->symbols();
        Tree->clear();
        u32 a = start;
        for (auto& [value, w] : rows)
        {
            QString by, fn;
            if (!w)
                by = "-";
            else if ((w & DMAWriterTag) == DMAWriterTag)
                by = QString("DMA%1 (ARM%2)").arg((w >> 1) & 3).arg(w & 1 ? 7 : 9);
            else
            {
                by = Hex(w & ~1u);
                fn = (w & 1 ? "[ARM7] " : "") + SymStr(syms.describe(w & ~1u));
            }
            QString name = SymStr(syms.describe(a));
            new QTreeWidgetItem(Tree, {Hex(a) + (name.isEmpty() ? "" : "  " + name), Hex(value), by, fn});
            a += 4;
        }
        for (int i = 0; i < Tree->columnCount(); i++)
            Tree->resizeColumnToContents(i);
    });
}
