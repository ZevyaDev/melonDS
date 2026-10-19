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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <set>

#include <QAbstractTableModel>
#include <QAction>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include "RamSearchWindow.h"
#include "DebugSymbols.h"
#include "DebugTools.h"

using namespace melonDS;

constexpr u32 MainRAMBase = 0x02000000;
constexpr u32 SharedWRAMBase = 0x03000000;
constexpr u32 ARM7WRAMBase = 0x03800000;
constexpr u32 ARM7WRAMMirrorMask = 0xFF800000;
constexpr u32 DTCMUnmapped = 0xFFFFFFFF;
constexpr double FixedOne = 4096.0;
constexpr size_t MaxUndo = 10;
constexpr int MaxLiveRows = 256;
constexpr int MaxWatchAdds = 256;

static const char* const TypeNames[] = {"signed", "unsigned", "hex", "float", "fixed"};

enum class RamRegion
{
    MainRAM,
    SharedWRAM,
    ARM7WRAM,
    DTCM,
};

enum class CompareTarget
{
    Previous,
    Value,
    Address,
    Changes,
};

enum class CompareOp
{
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    NotEqual,
    DifferentBy,
};

namespace SearchColumn
{
enum { Address, Symbol, Value, Previous, Changes, Count };
}

namespace WatchColumn
{
enum { Address, Resolved, Size, Type, Value, Notes, Count };
}

struct SearchFilter
{
    CompareTarget Target = CompareTarget::Previous;
    CompareOp Op = CompareOp::Equal;
    s64 Operand = 0;
    s64 By = 0;
};

struct RamSnapshot
{
    u32 Base = 0;
    u8 CPU = 0;
    QByteArray Data;
    u32 Reference = 0;
};

struct WatchRead
{
    bool Valid;
    u32 Addr;
    int Size;
};

static u8 CPUForAddress(u32 addr)
{
    return (addr & ARM7WRAMMirrorMask) == ARM7WRAMBase ? 1 : 0;
}

static u32 PeekValue(NDS& nds, u8 cpu, u32 addr, int size)
{
    if ((addr & (size - 1)) == 0)
        return nds.Debugger.Peek(cpu, addr, size);
    u32 value = 0;
    for (int i = 0; i < size; i++)
        value |= nds.Debugger.Peek(cpu, addr + i, 1) << (8 * i);
    return value;
}

static void PokeValue(NDS& nds, u8 cpu, u32 addr, int size, u32 value)
{
    if ((addr & (size - 1)) == 0)
    {
        nds.Debugger.Poke(cpu, addr, size, value);
        return;
    }
    for (int i = 0; i < size; i++)
        nds.Debugger.Poke(cpu, addr + i, 1, (value >> (8 * i)) & 0xFF);
}

static RamSnapshot TakeSnapshot(NDS& nds, RamRegion region)
{
    RamSnapshot snap;
    u32 size = 0;
    switch (region)
    {
    case RamRegion::MainRAM:
        snap.Base = MainRAMBase;
        snap.Data = QByteArray(reinterpret_cast<const char*>(nds.MainRAM), nds.MainRAMMask + 1);
        return snap;
    case RamRegion::SharedWRAM:
        snap.Base = SharedWRAMBase;
        size = nds.SharedWRAMSize;
        break;
    case RamRegion::ARM7WRAM:
        snap.Base = ARM7WRAMBase;
        snap.CPU = 1;
        size = nds.ARM7WRAMSize;
        break;
    case RamRegion::DTCM:
        if (nds.ARM9->DTCMBase == DTCMUnmapped)
            return snap;
        snap.Base = nds.ARM9->DTCMBase;
        size = DTCMPhysicalSize;
        break;
    }
    snap.Data.resize(size);
    char* out = snap.Data.data();
    for (u32 off = 0; off < size; off += 4)
    {
        u32 word = nds.Debugger.Peek(snap.CPU, snap.Base + off, 4);
        memcpy(out + off, &word, 4);
    }
    return snap;
}

static inline u32 ReadRaw(const QByteArray& data, u32 off, int size)
{
    const char* p = data.constData() + off;
    if (size == 1)
        return (u8)*p;
    if (size == 2)
    {
        u16 v;
        memcpy(&v, p, 2);
        return v;
    }
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static inline s64 Interpret(u32 raw, int size, bool sign)
{
    if (!sign)
        return raw;
    if (size == 1)
        return (s8)raw;
    if (size == 2)
        return (s16)raw;
    return (s32)raw;
}

static inline bool Matches(CompareOp op, s64 lhs, s64 rhs, s64 by)
{
    switch (op)
    {
    case CompareOp::Less: return lhs < rhs;
    case CompareOp::Greater: return lhs > rhs;
    case CompareOp::LessEqual: return lhs <= rhs;
    case CompareOp::GreaterEqual: return lhs >= rhs;
    case CompareOp::Equal: return lhs == rhs;
    case CompareOp::NotEqual: return lhs != rhs;
    case CompareOp::DifferentBy: return lhs - rhs == by || rhs - lhs == by;
    }
    return false;
}

static QString FormatValue(u32 raw, int size, RamValueType type)
{
    switch (type)
    {
    case RamValueType::Signed:
        return QString::number(Interpret(raw, size, true));
    case RamValueType::Unsigned:
        return QString::number(raw);
    case RamValueType::Hex:
        return Hex(raw, size * 2);
    case RamValueType::Float:
    {
        float f;
        memcpy(&f, &raw, 4);
        return QString::number(f, 'g', 9);
    }
    case RamValueType::Fixed:
        return QString::number(Interpret(raw, size, true) / FixedOne, 'f', 4);
    }
    return QString();
}

static bool ParseInteger(const QString& raw, bool hex, s64& out)
{
    QString text = raw.trimmed();
    bool negative = text.startsWith('-');
    if (negative || text.startsWith('+'))
        text = text.mid(1).trimmed();
    if (text.startsWith("0x", Qt::CaseInsensitive))
    {
        text = text.mid(2);
        hex = true;
    }
    bool ok;
    qulonglong magnitude = text.toULongLong(&ok, hex ? 16 : 10);
    if (!ok)
        return false;
    out = negative ? -(s64)magnitude : (s64)magnitude;
    return true;
}

static bool ParseRaw(const QString& text, int size, RamValueType type, u32& out)
{
    bool ok;
    s64 value = 0;
    switch (type)
    {
    case RamValueType::Float:
    {
        float f = text.trimmed().toFloat(&ok);
        if (!ok)
            return false;
        memcpy(&out, &f, 4);
        return true;
    }
    case RamValueType::Fixed:
    {
        double d = text.trimmed().toDouble(&ok);
        if (!ok)
            return false;
        value = std::llround(d * FixedOne);
        break;
    }
    default:
        if (!ParseInteger(text, type == RamValueType::Hex, value))
            return false;
        break;
    }
    u32 mask = size == 4 ? 0xFFFFFFFF : (1u << (8 * size)) - 1;
    out = (u32)value & mask;
    return true;
}

static QComboBox* SizeCombo(QWidget* parent)
{
    QComboBox* box = new QComboBox(parent);
    box->addItems({"1 byte", "2 bytes", "4 bytes"});
    return box;
}

static QComboBox* TypeCombo(QWidget* parent, bool withReal)
{
    QComboBox* box = new QComboBox(parent);
    box->addItems({"Signed", "Unsigned", "Hex"});
    if (withReal)
        box->addItems({"Float", "Fixed point"});
    return box;
}

static int SizeIndex(int size)
{
    return size == 1 ? 0 : size == 2 ? 1 : 2;
}

class RamSearchModel : public QAbstractTableModel
{
public:
    RamSearchModel(DebugBridge* bridge, QObject* parent) : QAbstractTableModel(parent), Bridge(bridge) {}

    std::function<void()> Poked;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override { return parent.isValid() ? 0 : (int)count(); }
    int columnCount(const QModelIndex& parent = QModelIndex()) const override { return parent.isValid() ? 0 : SearchColumn::Count; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        static const char* const names[] = {"Address", "Symbol", "Value", "Previous", "Changes"};
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < SearchColumn::Count)
            return QString(names[section]);
        return QVariant();
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || (size_t)index.row() >= count())
            return QVariant();
        size_t row = index.row();
        int column = index.column();
        if (role == Qt::TextAlignmentRole)
            return column >= SearchColumn::Value ? int(Qt::AlignRight | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
        if (role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::ForegroundRole)
            return QVariant();
        u32 previous = previousRaw(row);
        u32 current = previous;
        bool live = liveRaw(row, current);
        if (role == Qt::ForegroundRole)
            return column == SearchColumn::Value && live && current != previous ? QVariant(QBrush(Qt::red)) : QVariant();
        switch (column)
        {
        case SearchColumn::Address: return Hex(address(row));
        case SearchColumn::Symbol: return Bridge->symbols().describe(address(row));
        case SearchColumn::Value: return FormatValue(current, Size, Type);
        case SearchColumn::Previous: return FormatValue(previous, Size, Type);
        case SearchColumn::Changes: return changesAt(row);
        }
        return QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        if (index.column() == SearchColumn::Value)
            f |= Qt::ItemIsEditable;
        return f;
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (role != Qt::EditRole || index.column() != SearchColumn::Value || (size_t)index.row() >= count())
            return false;
        u32 raw;
        if (!ParseRaw(value.toString(), Size, Type, raw))
            return false;
        size_t row = index.row();
        u32 addr = address(row);
        u8 cpu = CPU;
        int size = Size;
        Bridge->post([cpu, addr, size, raw](NDS& nds) { PokeValue(nds, cpu, addr, size, raw); });
        if (row >= LiveFirst && row - LiveFirst < LiveValues.size())
            LiveValues[row - LiveFirst] = raw;
        emit dataChanged(index, index);
        if (Poked)
            Poked();
        return true;
    }

    bool started() const { return Started; }
    size_t count() const { return All ? Positions : Candidates.size(); }
    u32 address(size_t row) const { return Base + offsetAt(row); }
    u8 cpu() const { return CPU; }
    int valueSize() const { return Size; }
    u64 generation() const { return Generation; }
    bool canUndo() const { return !History.empty(); }
    qsizetype regionSize() const { return Previous.size(); }
    bool sameRegion(const RamSnapshot& snap) const { return snap.Base == Base && snap.Data.size() == Previous.size(); }
    s64 interpret(u32 raw) const { return Interpret(raw, Size, Type == RamValueType::Signed); }

    void setDisplay(RamValueType type)
    {
        Type = type;
        if (count())
            emit dataChanged(index(0, SearchColumn::Value), index(rowCount() - 1, SearchColumn::Previous));
    }

    void refreshSymbols()
    {
        if (count())
            emit dataChanged(index(0, SearchColumn::Symbol), index(rowCount() - 1, SearchColumn::Symbol));
    }

    void restart(const RamSnapshot& snap, int size, bool misaligned)
    {
        beginResetModel();
        Started = true;
        Base = snap.Base;
        CPU = snap.CPU;
        Size = size;
        Stride = misaligned ? 1 : size;
        Previous = snap.Data;
        Positions = Previous.size() >= size ? (size_t)(Previous.size() - size) / Stride + 1 : 0;
        All = true;
        std::vector<u32>().swap(Candidates);
        std::vector<u32>().swap(Changes);
        History.clear();
        invalidateLive();
        endResetModel();
    }

    void search(const RamSnapshot& snap, const SearchFilter& filter)
    {
        bool sign = Type == RamValueType::Signed;
        size_t n = count();
        std::vector<u32> kept;
        std::vector<u32> keptChanges;
        for (size_t row = 0; row < n; row++)
        {
            u32 off = offsetAt(row);
            u32 rawCurrent = ReadRaw(snap.Data, off, Size);
            u32 rawPrevious = ReadRaw(Previous, off, Size);
            u32 changes = changesAt(row) + (rawCurrent != rawPrevious ? 1 : 0);
            s64 lhs = filter.Target == CompareTarget::Changes ? (s64)changes : Interpret(rawCurrent, Size, sign);
            s64 rhs = filter.Target == CompareTarget::Previous ? Interpret(rawPrevious, Size, sign) : filter.Operand;
            if (Matches(filter.Op, lhs, rhs, filter.By))
            {
                kept.push_back(off);
                keptChanges.push_back(changes);
            }
        }
        beginResetModel();
        History.push_back({All, std::move(Candidates), std::move(Changes)});
        if (History.size() > MaxUndo)
            History.erase(History.begin());
        All = false;
        Candidates = std::move(kept);
        Changes = std::move(keptChanges);
        Previous = snap.Data;
        invalidateLive();
        endResetModel();
    }

    void undo()
    {
        if (History.empty())
            return;
        beginResetModel();
        Step& last = History.back();
        All = last.All;
        Candidates = std::move(last.Candidates);
        Changes = std::move(last.Changes);
        History.pop_back();
        invalidateLive();
        endResetModel();
    }

    void setLive(u64 generation, int first, const std::vector<u32>& values)
    {
        if (generation != Generation || values.empty() || first < 0 || first + values.size() > count())
            return;
        LiveFirst = first;
        LiveValues = values;
        emit dataChanged(index(first, SearchColumn::Value), index(first + (int)values.size() - 1, SearchColumn::Value));
    }

private:
    struct Step
    {
        bool All;
        std::vector<u32> Candidates;
        std::vector<u32> Changes;
    };

    DebugBridge* Bridge;
    bool Started = false;
    u32 Base = 0;
    u8 CPU = 0;
    int Size = 4;
    u32 Stride = 4;
    size_t Positions = 0;
    RamValueType Type = RamValueType::Unsigned;
    QByteArray Previous;
    bool All = true;
    std::vector<u32> Candidates;
    std::vector<u32> Changes;
    std::vector<Step> History;
    u64 Generation = 0;
    size_t LiveFirst = 0;
    std::vector<u32> LiveValues;

    u32 offsetAt(size_t row) const { return All ? (u32)(row * Stride) : Candidates[row]; }
    u32 changesAt(size_t row) const { return All ? 0 : Changes[row]; }
    u32 previousRaw(size_t row) const { return ReadRaw(Previous, offsetAt(row), Size); }

    bool liveRaw(size_t row, u32& out) const
    {
        if (row < LiveFirst || row - LiveFirst >= LiveValues.size())
            return false;
        out = LiveValues[row - LiveFirst];
        return true;
    }

    void invalidateLive()
    {
        Generation++;
        LiveValues.clear();
    }
};

RamSearchWindow::RamSearchWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "RAM search", parent)
{
    resize(820, 640);
    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* format = new QHBoxLayout();
    Region = new QComboBox(this);
    Region->addItems({"Main RAM", "Shared WRAM (ARM9)", "ARM7 WRAM", "DTCM"});
    Size = SizeCombo(this);
    Size->setCurrentIndex(SizeIndex(4));
    Display = TypeCombo(this, false);
    Display->setCurrentIndex((int)RamValueType::Unsigned);
    Misaligned = new QCheckBox("Misaligned", this);
    format->addWidget(new QLabel("Region", this));
    format->addWidget(Region);
    format->addWidget(new QLabel("Size", this));
    format->addWidget(Size);
    format->addWidget(new QLabel("Display", this));
    format->addWidget(Display);
    format->addWidget(Misaligned);
    format->addStretch();
    layout->addLayout(format);

    QHBoxLayout* compare = new QHBoxLayout();
    Operator = new QComboBox(this);
    Operator->addItems({"<", ">", "<=", ">=", "==", "!=", "different by"});
    Operator->setCurrentIndex((int)CompareOp::NotEqual);
    CompareTo = new QComboBox(this);
    CompareTo->addItems({"Previous value", "Specific value", "Specific address", "Number of changes"});
    Operand = new QLineEdit(this);
    DifferentBy = new QLineEdit(this);
    DifferentBy->setPlaceholderText("N");
    DifferentBy->setMaximumWidth(100);
    compare->addWidget(new QLabel("Value", this));
    compare->addWidget(Operator);
    compare->addWidget(CompareTo);
    compare->addWidget(Operand, 1);
    compare->addWidget(new QLabel("by", this));
    compare->addWidget(DifferentBy);
    layout->addLayout(compare);

    QHBoxLayout* actions = new QHBoxLayout();
    QPushButton* reset = new QPushButton("Reset", this);
    QPushButton* searchButton = new QPushButton("Search", this);
    UndoButton = new QPushButton("Undo", this);
    UndoButton->setEnabled(false);
    QPushButton* watch = new QPushButton("Add to watch", this);
    actions->addWidget(reset);
    actions->addWidget(searchButton);
    actions->addWidget(UndoButton);
    actions->addStretch();
    actions->addWidget(watch);
    layout->addLayout(actions);

    Model = new RamSearchModel(bridge, this);
    Model->Poked = [this] { requestRefresh(); };
    Table = new QTableView(this);
    Table->setModel(Model);
    Table->setFont(MonoFont());
    Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    Table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    Table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    Table->setWordWrap(false);
    Table->verticalHeader()->hide();
    Table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    Table->verticalHeader()->setDefaultSectionSize(Table->fontMetrics().height() + 2);
    QHeaderView* header = Table->horizontalHeader();
    int digit = Table->fontMetrics().horizontalAdvance('0');
    header->resizeSection(SearchColumn::Address, digit * 11);
    header->resizeSection(SearchColumn::Value, digit * 13);
    header->resizeSection(SearchColumn::Previous, digit * 13);
    header->resizeSection(SearchColumn::Changes, digit * 9);
    header->setSectionResizeMode(SearchColumn::Symbol, QHeaderView::Stretch);
    layout->addWidget(Table);

    Info = new QLabel(this);
    layout->addWidget(Info);

    connect(Region, &QComboBox::currentIndexChanged, this, &RamSearchWindow::restart);
    connect(Size, &QComboBox::currentIndexChanged, this, &RamSearchWindow::restart);
    connect(Misaligned, &QCheckBox::toggled, this, &RamSearchWindow::restart);
    connect(Display, &QComboBox::currentIndexChanged, this, [this](int index) {
        Model->setDisplay((RamValueType)index);
    });
    connect(Operator, &QComboBox::currentIndexChanged, this, &RamSearchWindow::updateControls);
    connect(CompareTo, &QComboBox::currentIndexChanged, this, &RamSearchWindow::updateControls);
    connect(Operand, &QLineEdit::returnPressed, this, &RamSearchWindow::search);
    connect(DifferentBy, &QLineEdit::returnPressed, this, &RamSearchWindow::search);
    connect(reset, &QPushButton::clicked, this, &RamSearchWindow::restart);
    connect(searchButton, &QPushButton::clicked, this, &RamSearchWindow::search);
    connect(UndoButton, &QPushButton::clicked, this, &RamSearchWindow::undo);
    connect(watch, &QPushButton::clicked, this, &RamSearchWindow::addToWatch);
    connect(Table->verticalScrollBar(), &QScrollBar::valueChanged, this, &RamSearchWindow::requestRefresh);
    connect(Table, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() != SearchColumn::Value)
            addRowToWatch(index.row());
    });
    connect(bridge, &DebugBridge::symbolsChanged, this, [this] { Model->refreshSymbols(); });

    updateControls();
}

void RamSearchWindow::updateControls()
{
    CompareTarget target = (CompareTarget)CompareTo->currentIndex();
    Operand->setEnabled(target != CompareTarget::Previous);
    switch (target)
    {
    case CompareTarget::Previous: Operand->setPlaceholderText(QString()); break;
    case CompareTarget::Value: Operand->setPlaceholderText("value"); break;
    case CompareTarget::Address: Operand->setPlaceholderText("address or symbol+offset"); break;
    case CompareTarget::Changes: Operand->setPlaceholderText("count"); break;
    }
    DifferentBy->setEnabled((CompareOp)Operator->currentIndex() == CompareOp::DifferentBy);
}

void RamSearchWindow::showCount(const QString& prefix)
{
    QString text = QString("%1 candidates").arg(QLocale().toString((qulonglong)Model->count()));
    Info->setText(prefix.isEmpty() ? text : prefix + "; " + text);
    UndoButton->setEnabled(Model->canUndo());
}

void RamSearchWindow::restart()
{
    if (!Bridge->hasConsole())
    {
        Info->setText("No game is running");
        return;
    }
    RamRegion region = (RamRegion)Region->currentIndex();
    int size = 1 << Size->currentIndex();
    bool misaligned = Misaligned->isChecked();
    Restarting = true;
    Bridge->call(this, [region](NDS& nds) { return TakeSnapshot(nds, region); },
        [this, region, size, misaligned](const RamSnapshot& snap) {
            Restarting = false;
            Model->restart(snap, size, misaligned);
            Table->scrollToTop();
            if (snap.Data.isEmpty())
                Info->setText(region == RamRegion::DTCM ? "DTCM is not mapped" : "Region is empty");
            else
                showCount(QString("Started at %1, %2 KB").arg(Hex(snap.Base)).arg(snap.Data.size() / 1024));
            requestRefresh();
        });
}

void RamSearchWindow::search()
{
    if (!Model->started())
    {
        restart();
        return;
    }
    if (!Bridge->hasConsole())
    {
        Info->setText("No game is running");
        return;
    }
    SearchFilter filter;
    filter.Target = (CompareTarget)CompareTo->currentIndex();
    filter.Op = (CompareOp)Operator->currentIndex();
    bool hex = (RamValueType)Display->currentIndex() == RamValueType::Hex;
    u32 reference = 0;
    switch (filter.Target)
    {
    case CompareTarget::Previous:
        break;
    case CompareTarget::Value:
        if (!ParseInteger(Operand->text(), hex, filter.Operand))
        {
            Info->setText("Enter a value to compare to");
            return;
        }
        break;
    case CompareTarget::Address:
        if (!Bridge->symbols().parseAddress(Operand->text(), reference))
        {
            Info->setText("Enter an address or symbol to compare to");
            return;
        }
        break;
    case CompareTarget::Changes:
        if (!ParseInteger(Operand->text(), false, filter.Operand))
        {
            Info->setText("Enter a number of changes");
            return;
        }
        break;
    }
    if (filter.Op == CompareOp::DifferentBy)
    {
        if (!ParseInteger(DifferentBy->text(), hex && filter.Target != CompareTarget::Changes, filter.By))
        {
            Info->setText("Enter the difference N");
            return;
        }
        filter.By = std::llabs(filter.By);
    }
    RamRegion region = (RamRegion)Region->currentIndex();
    int size = Model->valueSize();
    bool useReference = filter.Target == CompareTarget::Address;
    qulonglong before = Model->count();
    Bridge->call(this, [region, useReference, reference, size](NDS& nds) {
        RamSnapshot snap = TakeSnapshot(nds, region);
        if (useReference)
            snap.Reference = PeekValue(nds, CPUForAddress(reference), reference, size);
        return snap;
    }, [this, filter, before](const RamSnapshot& snap) {
        if (!Model->sameRegion(snap))
        {
            Model->restart(snap, 1 << Size->currentIndex(), Misaligned->isChecked());
            showCount("The region moved or changed size, so the search restarted");
            return;
        }
        SearchFilter applied = filter;
        if (applied.Target == CompareTarget::Address)
            applied.Operand = Model->interpret(snap.Reference);
        Model->search(snap, applied);
        Table->scrollToTop();
        showCount(QString("Was %1").arg(QLocale().toString(before)));
        requestRefresh();
    });
}

void RamSearchWindow::undo()
{
    Model->undo();
    showCount("Undone");
    requestRefresh();
}

void RamSearchWindow::addRowToWatch(int row)
{
    if (row < 0 || (size_t)row >= Model->count())
        return;
    RamWatchWindow::addWatch(Bridge, parentWidget(), Model->address(row), Model->valueSize(), (RamValueType)Display->currentIndex());
}

void RamSearchWindow::addToWatch()
{
    std::set<int> rows;
    size_t selected = 0;
    for (const QItemSelectionRange& range : Table->selectionModel()->selection())
    {
        selected += range.height();
        for (int r = range.top(); r <= range.bottom() && rows.size() < (size_t)MaxWatchAdds; r++)
            rows.insert(r);
    }
    if (rows.empty())
    {
        Info->setText("Select candidates to add them to the watch list");
        return;
    }
    for (int r : rows)
        addRowToWatch(r);
    if (selected > rows.size())
        Info->setText(QString("Added the first %1 selected candidates").arg(rows.size()));
}

void RamSearchWindow::refresh()
{
    if (!Model->started())
    {
        if (!Restarting)
            restart();
        return;
    }
    int total = Model->rowCount();
    int first = Table->rowAt(0);
    if (total == 0 || first < 0)
        return;
    int last = Table->rowAt(Table->viewport()->height() - 1);
    if (last < 0)
        last = total - 1;
    last = std::min(last, first + MaxLiveRows - 1);
    std::vector<u32> addrs;
    addrs.reserve(last - first + 1);
    for (int r = first; r <= last; r++)
        addrs.push_back(Model->address(r));
    u8 cpu = Model->cpu();
    int size = Model->valueSize();
    u64 generation = Model->generation();
    fetch([addrs, cpu, size](NDS& nds) {
        std::vector<u32> values;
        values.reserve(addrs.size());
        for (u32 a : addrs)
            values.push_back(PeekValue(nds, cpu, a, size));
        return values;
    }, [this, generation, first](const std::vector<u32>& values) {
        Model->setLive(generation, first, values);
        if (Table->rowAt(0) != first)
            requestRefresh();
    });
}

RamWatchWindow::RamWatchWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "RAM watch", parent)
{
    resize(820, 420);
    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* entry = new QHBoxLayout();
    NewAddr = new QLineEdit(this);
    NewAddr->setPlaceholderText("address or symbol+offset");
    NewSize = SizeCombo(this);
    NewSize->setCurrentIndex(SizeIndex(4));
    NewType = TypeCombo(this, true);
    NewType->setCurrentIndex((int)RamValueType::Hex);
    NewNotes = new QLineEdit(this);
    NewNotes->setPlaceholderText("notes");
    QPushButton* add = new QPushButton("Add", this);
    entry->addWidget(NewAddr, 2);
    entry->addWidget(NewSize);
    entry->addWidget(NewType);
    entry->addWidget(NewNotes, 3);
    entry->addWidget(add);
    layout->addLayout(entry);

    Table = new QTableWidget(0, WatchColumn::Count, this);
    Table->setHorizontalHeaderLabels({"Address", "Resolved", "Size", "Type", "Value", "Notes"});
    Table->setFont(MonoFont());
    Table->verticalHeader()->hide();
    Table->horizontalHeader()->setStretchLastSection(true);
    Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    Table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    Table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    int digit = Table->fontMetrics().horizontalAdvance('0');
    Table->horizontalHeader()->resizeSection(WatchColumn::Address, digit * 18);
    Table->horizontalHeader()->resizeSection(WatchColumn::Resolved, digit * 30);
    Table->horizontalHeader()->resizeSection(WatchColumn::Value, digit * 14);
    layout->addWidget(Table);

    QHBoxLayout* buttons = new QHBoxLayout();
    QPushButton* remove = new QPushButton("Remove", this);
    QPushButton* up = new QPushButton("Move up", this);
    QPushButton* down = new QPushButton("Move down", this);
    QPushButton* loadButton = new QPushButton("Load...", this);
    QPushButton* saveButton = new QPushButton("Save...", this);
    buttons->addWidget(remove);
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addStretch();
    buttons->addWidget(loadButton);
    buttons->addWidget(saveButton);
    layout->addLayout(buttons);

    Info = new QLabel("Double-click a value to change it in memory.", this);
    layout->addWidget(Info);

    QAction* removeAction = new QAction(Table);
    removeAction->setShortcut(QKeySequence::Delete);
    removeAction->setShortcutContext(Qt::WidgetShortcut);
    Table->addAction(removeAction);

    connect(add, &QPushButton::clicked, this, &RamWatchWindow::addFromInputs);
    connect(NewAddr, &QLineEdit::returnPressed, this, &RamWatchWindow::addFromInputs);
    connect(NewNotes, &QLineEdit::returnPressed, this, &RamWatchWindow::addFromInputs);
    connect(NewType, &QComboBox::currentIndexChanged, this, [this](int index) {
        if ((RamValueType)index == RamValueType::Float)
            NewSize->setCurrentIndex(SizeIndex(4));
    });
    connect(remove, &QPushButton::clicked, this, &RamWatchWindow::removeSelected);
    connect(removeAction, &QAction::triggered, this, &RamWatchWindow::removeSelected);
    connect(up, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    connect(loadButton, &QPushButton::clicked, this, &RamWatchWindow::load);
    connect(saveButton, &QPushButton::clicked, this, &RamWatchWindow::save);
    connect(Table, &QTableWidget::itemChanged, this, &RamWatchWindow::onItemChanged);
    connect(bridge, &DebugBridge::symbolsChanged, this, [this] {
        for (size_t i = 0; i < Watches.size(); i++)
        {
            resolve(Watches[i]);
            showRow(i);
        }
    });
    setInterval(100);
}

void RamWatchWindow::addWatch(DebugBridge* bridge, QWidget* parent, u32 addr, int size, RamValueType type)
{
    RamWatchWindow* win = DebugTools::open<RamWatchWindow>(bridge, parent);
    int existing = win->find(addr, size);
    if (existing >= 0)
    {
        win->Table->selectRow(existing);
        return;
    }
    win->append(Hex(addr), size, type, QString());
}

void RamWatchWindow::append(const QString& expr, int size, RamValueType type, const QString& notes)
{
    Watch w;
    w.Expr = expr;
    w.Size = type == RamValueType::Float ? 4 : size;
    w.Type = type;
    w.Notes = notes;
    resolve(w);
    Watches.push_back(w);
    int row = Table->rowCount();
    Table->insertRow(row);
    setupRow(row);
    Table->scrollToItem(Table->item(row, WatchColumn::Address));
    requestRefresh();
}

int RamWatchWindow::find(u32 addr, int size) const
{
    for (size_t i = 0; i < Watches.size(); i++)
        if (Watches[i].Valid && Watches[i].Addr == addr && Watches[i].Size == size)
            return i;
    return -1;
}

void RamWatchWindow::resolve(Watch& w)
{
    u32 old = w.Addr;
    bool wasValid = w.Valid;
    w.Valid = Bridge->symbols().parseAddress(w.Expr, w.Addr);
    if (!w.Valid || !wasValid || w.Addr != old)
        w.Known = false;
}

int RamWatchWindow::rowOf(QWidget* widget, int column) const
{
    for (int r = 0; r < Table->rowCount(); r++)
        if (Table->cellWidget(r, column) == widget)
            return r;
    return -1;
}

void RamWatchWindow::setupRow(int row)
{
    Filling = true;
    for (int column : {WatchColumn::Address, WatchColumn::Resolved, WatchColumn::Value, WatchColumn::Notes})
        Table->setItem(row, column, new QTableWidgetItem());
    Table->item(row, WatchColumn::Resolved)->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    Filling = false;

    QComboBox* size = SizeCombo(Table);
    QComboBox* type = TypeCombo(Table, true);
    Table->setCellWidget(row, WatchColumn::Size, size);
    Table->setCellWidget(row, WatchColumn::Type, type);
    connect(size, &QComboBox::currentIndexChanged, this, [this, size](int index) {
        int r = rowOf(size, WatchColumn::Size);
        if (r < 0)
            return;
        Watch& w = Watches[r];
        w.Size = w.Type == RamValueType::Float ? 4 : 1 << index;
        w.Known = false;
        showRow(r);
        requestRefresh();
    });
    connect(type, &QComboBox::currentIndexChanged, this, [this, type](int index) {
        int r = rowOf(type, WatchColumn::Type);
        if (r < 0)
            return;
        Watch& w = Watches[r];
        w.Type = (RamValueType)index;
        if (w.Type == RamValueType::Float && w.Size != 4)
        {
            w.Size = 4;
            w.Known = false;
            requestRefresh();
        }
        showRow(r);
    });
    showRow(row);
}

void RamWatchWindow::showRow(int row)
{
    const Watch& w = Watches[row];
    QString symbol = w.Valid ? Bridge->symbols().describe(w.Addr) : QString();
    Filling = true;
    Table->item(row, WatchColumn::Address)->setText(w.Expr);
    Table->item(row, WatchColumn::Resolved)->setText(!w.Valid ? QString("?") : symbol.isEmpty() ? Hex(w.Addr) : Hex(w.Addr) + "  " + symbol);
    Table->item(row, WatchColumn::Notes)->setText(w.Notes);
    Filling = false;
    QComboBox* size = static_cast<QComboBox*>(Table->cellWidget(row, WatchColumn::Size));
    QComboBox* type = static_cast<QComboBox*>(Table->cellWidget(row, WatchColumn::Type));
    {
        QSignalBlocker blockSize(size);
        QSignalBlocker blockType(type);
        size->setCurrentIndex(SizeIndex(w.Size));
        type->setCurrentIndex((int)w.Type);
    }
    showValue(row);
}

void RamWatchWindow::showValue(int row)
{
    const Watch& w = Watches[row];
    Filling = true;
    Table->item(row, WatchColumn::Value)->setText(!w.Valid ? QString("bad address") : w.Known ? FormatValue(w.Raw, w.Size, w.Type) : QString());
    Filling = false;
}

void RamWatchWindow::pokeValue(int row, const QString& text)
{
    Watch& w = Watches[row];
    u32 raw;
    if (!w.Valid || !ParseRaw(text, w.Size, w.Type, raw))
    {
        Info->setText(QString("Cannot write \"%1\" as %2").arg(text.trimmed()).arg(QString(TypeNames[(int)w.Type])));
        showValue(row);
        return;
    }
    u32 addr = w.Addr;
    int size = w.Size;
    Bridge->post([addr, size, raw](NDS& nds) { PokeValue(nds, CPUForAddress(addr), addr, size, raw); });
    w.Raw = raw;
    w.Known = true;
    showValue(row);
    Info->setText(QString("Wrote %1 to %2").arg(Hex(raw, size * 2)).arg(Hex(addr)));
    requestRefresh();
}

void RamWatchWindow::onItemChanged(QTableWidgetItem* item)
{
    if (Filling)
        return;
    int row = item->row();
    if (row < 0 || row >= (int)Watches.size())
        return;
    Watch& w = Watches[row];
    switch (item->column())
    {
    case WatchColumn::Address:
        w.Expr = item->text().trimmed();
        resolve(w);
        showRow(row);
        requestRefresh();
        break;
    case WatchColumn::Value:
        pokeValue(row, item->text());
        break;
    case WatchColumn::Notes:
        w.Notes = item->text();
        break;
    }
}

void RamWatchWindow::addFromInputs()
{
    QString expr = NewAddr->text().trimmed();
    if (expr.isEmpty())
        return;
    append(expr, 1 << NewSize->currentIndex(), (RamValueType)NewType->currentIndex(), NewNotes->text().trimmed());
    Info->setText(Watches.back().Valid ? QString() : expr + " does not resolve to an address yet");
    NewAddr->clear();
    NewNotes->clear();
    NewAddr->setFocus();
}

void RamWatchWindow::removeSelected()
{
    std::vector<int> rows;
    for (const QModelIndex& index : Table->selectionModel()->selectedRows())
        rows.push_back(index.row());
    std::sort(rows.rbegin(), rows.rend());
    for (int r : rows)
    {
        Table->removeRow(r);
        Watches.erase(Watches.begin() + r);
    }
}

void RamWatchWindow::moveCurrent(int delta)
{
    int row = Table->currentRow();
    int target = row + delta;
    if (row < 0 || target < 0 || target >= (int)Watches.size())
        return;
    std::swap(Watches[row], Watches[target]);
    showRow(row);
    showRow(target);
    Table->selectRow(target);
}

void RamWatchWindow::save()
{
    QString path = QFileDialog::getSaveFileName(this, "Save watches", "watches.txt", "Text (*.txt)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "melonDS", "Cannot write " + path);
        return;
    }
    QTextStream out(&f);
    for (const Watch& w : Watches)
    {
        QString expr = w.Expr;
        QString notes = w.Notes;
        out << expr.replace('\t', ' ') << '\t' << w.Size << '\t' << TypeNames[(int)w.Type] << '\t' << notes.replace('\t', ' ') << '\n';
    }
    Info->setText(QString("Saved %1 watches to %2").arg(Watches.size()).arg(path));
}

void RamWatchWindow::load()
{
    QString path = QFileDialog::getOpenFileName(this, "Load watches", QString(), "Text (*.txt);;All files (*)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "melonDS", "Cannot read " + path);
        return;
    }
    Table->setRowCount(0);
    Watches.clear();
    QTextStream in(&f);
    int skipped = 0;
    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.trimmed().isEmpty())
            continue;
        QStringList fields = line.contains('\t') ? line.split('\t') : line.simplified().split(' ');
        bool ok = fields.size() >= 3;
        int size = ok ? fields[1].trimmed().toInt(&ok) : 0;
        int type = -1;
        for (int i = 0; ok && i < (int)std::size(TypeNames); i++)
            if (fields[2].trimmed().compare(QString(TypeNames[i]), Qt::CaseInsensitive) == 0)
                type = i;
        if (!ok || (size != 1 && size != 2 && size != 4) || type < 0 || fields[0].trimmed().isEmpty())
        {
            skipped++;
            continue;
        }
        append(fields[0].trimmed(), size, (RamValueType)type, fields.mid(3).join(' ').trimmed());
    }
    Info->setText(skipped ? QString("Loaded %1 watches, skipped %2 unreadable lines").arg(Watches.size()).arg(skipped)
                          : QString("Loaded %1 watches").arg(Watches.size()));
}

void RamWatchWindow::refresh()
{
    if (Watches.empty())
        return;
    std::vector<WatchRead> reads;
    reads.reserve(Watches.size());
    for (const Watch& w : Watches)
        reads.push_back({w.Valid, w.Addr, w.Size});
    fetch([reads](NDS& nds) {
        std::vector<u32> values;
        values.reserve(reads.size());
        for (const WatchRead& r : reads)
            values.push_back(r.Valid ? PeekValue(nds, CPUForAddress(r.Addr), r.Addr, r.Size) : 0);
        return values;
    }, [this, reads](const std::vector<u32>& values) {
        for (size_t i = 0; i < values.size() && i < Watches.size(); i++)
        {
            Watch& w = Watches[i];
            if (!w.Valid || !reads[i].Valid || w.Addr != reads[i].Addr || w.Size != reads[i].Size)
                continue;
            if (w.Known && w.Raw == values[i])
                continue;
            w.Raw = values[i];
            w.Known = true;
            showValue(i);
        }
    });
}
