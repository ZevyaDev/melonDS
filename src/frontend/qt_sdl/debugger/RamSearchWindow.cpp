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
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include "RamSearchWindow.h"
#include "DebugSymbolsQt.h"
#include "DebugTools.h"
#include "EmuInstance.h"
#include "Config.h"
#include "NDSCart.h"

using namespace melonDS;

constexpr u32 MainRAMBase = 0x02000000;
constexpr u32 SharedWRAMBase = 0x03000000;
constexpr u32 ARM7WRAMBase = 0x03800000;
constexpr u32 ARM7WRAMMirrorMask = 0xFF800000;
constexpr u32 DTCMUnmapped = 0xFFFFFFFF;
constexpr u32 ARRegionEnd = 0x02FFFFFF;
constexpr u32 ARAddressMask = 0x0FFFFFFF;
constexpr double FixedOne = 4096.0;
constexpr size_t MaxUndo = 10;
constexpr int MaxLiveRows = 256;
constexpr int MaxWatchAdds = 256;
constexpr size_t MaxCheatAdds = 16;
constexpr qint64 AutoSearchMs = 200;
constexpr qint64 StaleSearchMs = 5000;
constexpr int MaxRecentWatchFiles = 8;

static const char* const TypeNames[] = {"signed", "unsigned", "hex", "float", "fixed"};
static const char* const SeparatorType = "separator";
static const char* const SeparatorNotes = "----------------";
static const char* const RecentWatchKey = "Debugger.WatchRecent";
static const char* const WatchGameKey = "Debugger.Watch.";

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
    Modulo,
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
    u32 Frame = 0;
};

struct WatchRead
{
    bool Valid;
    u32 Addr;
    int Size;
};

struct CheatDraft
{
    u32 Addr;
    int Size;
    u32 Value;
    QString Name;
};

struct CheatSource
{
    u32 Addr;
    int Size;
    QString Name;
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
    snap.Frame = nds.NumFrames;
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
    case CompareOp::Modulo: return by != 0 && lhs % by == rhs;
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

static QString GameCode(DebugBridge* bridge)
{
    NDS* nds = bridge->instance()->getNDS();
    if (!nds || !nds->GetNDSCart())
        return QString();
    return QString::fromLatin1(nds->GetNDSCart()->GetHeader().GameCode, 4);
}

static QStringList RecentWatchFiles(DebugBridge* bridge)
{
    QString joined = QString::fromStdString(bridge->instance()->getGlobalConfig().GetString(RecentWatchKey));
    return joined.split('\n', Qt::SkipEmptyParts);
}

static bool BuildARCode(u32 addr, int size, u32 value, std::vector<u32>& code, QString& error)
{
    if (addr < MainRAMBase || addr > ARRegionEnd)
    {
        error = "AR constant writes reach main RAM only (02000000-02FFFFFF)";
        return false;
    }
    if (addr & (size - 1))
    {
        error = QString("A %1-byte write needs a %1-aligned address").arg(size);
        return false;
    }
    u32 op = size == 4 ? 0x0 : size == 2 ? 0x1 : 0x2;
    u32 mask = size == 4 ? 0xFFFFFFFF : (1u << (8 * size)) - 1;
    code = {(op << 28) | (addr & ARAddressMask), value & mask};
    return true;
}

static QString CodeLine(const std::vector<u32>& code)
{
    return QString::asprintf("%08X %08X", code[0], code[1]);
}

static bool ParseCheatValue(const QString& text, int size, u32& out)
{
    s64 value;
    if (!ParseInteger(text, false, value))
        return false;
    s64 max = size == 4 ? 0xFFFFFFFFll : (1ll << (8 * size)) - 1;
    s64 min = -(1ll << (8 * size - 1));
    if (value < min || value > max)
        return false;
    out = (u32)value & (u32)max;
    return true;
}

static QString CheatName(DebugBridge* bridge, u32 addr, const QString& notes)
{
    if (!notes.trimmed().isEmpty())
        return notes.trimmed();
    QString symbol = SymStr(bridge->symbols().describe(addr));
    return symbol.isEmpty() ? "RAM " + Hex(addr) : symbol;
}

static bool EditCheat(DebugBridge* bridge, QWidget* parent, CheatDraft& draft, bool& enabled)
{
    std::vector<u32> code;
    QDialog dlg(parent);
    dlg.setWindowTitle("Add cheat");
    QFormLayout* form = new QFormLayout(&dlg);
    QLineEdit* name = new QLineEdit(draft.Name, &dlg);
    QLineEdit* addr = new QLineEdit(Hex(draft.Addr), &dlg);
    QComboBox* size = SizeCombo(&dlg);
    size->setCurrentIndex(SizeIndex(draft.Size));
    QLineEdit* value = new QLineEdit("0x" + Hex(draft.Value, draft.Size * 2), &dlg);
    value->setToolTip("Decimal, negative, or 0x-prefixed hex");
    QCheckBox* on = new QCheckBox("Enabled", &dlg);
    on->setChecked(true);
    QLabel* preview = new QLabel(&dlg);
    preview->setFont(MonoFont());
    preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow("Name", name);
    form->addRow("Address", addr);
    form->addRow("Size", size);
    form->addRow("Value", value);
    form->addRow(QString(), on);
    form->addRow("AR code", preview);
    form->addRow(buttons);
    auto update = [&] {
        CheatDraft next = draft;
        next.Size = 1 << size->currentIndex();
        QString error;
        bool ok = false;
        if (!bridge->symbols().parseAddress(SymStr(addr->text()), next.Addr))
            error = "Enter an address or symbol";
        else if (!ParseCheatValue(value->text(), next.Size, next.Value))
            error = "Enter a value that fits the size";
        else
            ok = BuildARCode(next.Addr, next.Size, next.Value, code, error);
        preview->setText(ok ? CodeLine(code) : error);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(ok && !name->text().trimmed().isEmpty());
        if (ok)
            draft = next;
    };
    QObject::connect(name, &QLineEdit::textChanged, &dlg, update);
    QObject::connect(addr, &QLineEdit::textChanged, &dlg, update);
    QObject::connect(value, &QLineEdit::textChanged, &dlg, update);
    QObject::connect(size, &QComboBox::currentIndexChanged, &dlg, update);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    update();
    value->setFocus();
    value->selectAll();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    draft.Name = name->text().trimmed();
    enabled = on->isChecked();
    return true;
}

static QString CommitCheat(DebugBridge* bridge, QWidget* parent, const CheatDraft& draft, bool enabled)
{
    std::vector<u32> code;
    QString error;
    if (!BuildARCode(draft.Addr, draft.Size, draft.Value, code, error))
        return error;
    EmuInstance* inst = bridge->instance();
    ARCodeFile* file = inst->getCheatFile();
    if (!file)
        return "No game is loaded, so there is no cheat file to add to";
    if (file->Error)
        return "The game's cheat file could not be read; fix it in Setup cheat codes first";
    ARCode ar {};
    ar.Parent = &file->RootCat;
    ar.Name = draft.Name.toStdString();
    ar.Enabled = enabled;
    ar.Code = code;
    file->RootCat.Children.emplace_back(ar);
    QString line = CodeLine(code);
    if (!file->Save())
        return "Added " + line + ", but the cheat file could not be written";
    if (!enabled)
        return "Added " + line + " (disabled)";
    if (!inst->getLocalConfig().GetBool("EnableCheats"))
    {
        MainWindow* win = inst->getMainWindow();
        if (win && win->actEnableCheats->isEnabled()
            && QMessageBox::question(parent, "melonDS", "Cheats are turned off. Turn them on now?") == QMessageBox::Yes)
        {
            win->actEnableCheats->trigger();
            return "Added " + line + " and turned cheats on";
        }
        return "Added " + line + "; it runs once cheats are turned on";
    }
    std::vector<ARCode> codes = file->GetCodes();
    bridge->post([codes](NDS& nds) { nds.AREngine.Cheats = codes; });
    return "Added " + line + " and it is running";
}

static void AddCheats(DebugBridge* bridge, QWidget* parent, QLabel* info, const std::vector<CheatSource>& sources)
{
    if (!bridge->hasConsole())
    {
        info->setText("No game is running");
        return;
    }
    std::vector<WatchRead> reads;
    for (const CheatSource& s : sources)
        reads.push_back({true, s.Addr, s.Size});
    bridge->call(parent, [reads](NDS& nds) {
        std::vector<u32> values;
        values.reserve(reads.size());
        for (const WatchRead& r : reads)
            values.push_back(PeekValue(nds, CPUForAddress(r.Addr), r.Addr, r.Size));
        return values;
    }, [bridge, parent, info, sources](const std::vector<u32>& values) {
        QString status;
        for (size_t i = 0; i < sources.size() && i < values.size(); i++)
        {
            CheatDraft draft {sources[i].Addr, sources[i].Size, values[i], sources[i].Name};
            bool enabled = true;
            if (!EditCheat(bridge, parent, draft, enabled))
                break;
            status = CommitCheat(bridge, parent, draft, enabled);
        }
        if (!status.isEmpty())
            info->setText(status);
    });
}

static void OpenCheatList(DebugBridge* bridge)
{
    MainWindow* win = bridge->instance()->getMainWindow();
    if (win && win->actSetupCheats->isEnabled())
        win->actSetupCheats->trigger();
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
        case SearchColumn::Symbol: return SymStr(Bridge->symbols().describe(address(row)));
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

    void search(const RamSnapshot& snap, const SearchFilter& filter, bool recordUndo)
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
        if (!recordUndo && !All && kept.size() == n)
        {
            Changes = std::move(keptChanges);
            Previous = snap.Data;
            invalidateLive();
            if (n)
                emit dataChanged(index(0, SearchColumn::Value), index((int)n - 1, SearchColumn::Changes));
            return;
        }
        beginResetModel();
        if (recordUndo)
            remember();
        All = false;
        Candidates = std::move(kept);
        Changes = std::move(keptChanges);
        Previous = snap.Data;
        invalidateLive();
        endResetModel();
    }

    void eliminate(const std::vector<std::pair<size_t, size_t>>& ranges)
    {
        size_t n = count();
        std::vector<u32> kept;
        std::vector<u32> keptChanges;
        size_t r = 0;
        for (size_t row = 0; row < n; row++)
        {
            while (r < ranges.size() && ranges[r].second < row)
                r++;
            if (r < ranges.size() && ranges[r].first <= row)
                continue;
            kept.push_back(offsetAt(row));
            keptChanges.push_back(changesAt(row));
        }
        if (kept.size() == n)
            return;
        beginResetModel();
        remember();
        All = false;
        Candidates = std::move(kept);
        Changes = std::move(keptChanges);
        invalidateLive();
        endResetModel();
    }

    void clearChanges()
    {
        if (All || Changes.empty())
            return;
        std::fill(Changes.begin(), Changes.end(), 0);
        emit dataChanged(index(0, SearchColumn::Changes), index(rowCount() - 1, SearchColumn::Changes));
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

    void remember()
    {
        History.push_back({All, std::move(Candidates), std::move(Changes)});
        if (History.size() > MaxUndo)
            History.erase(History.begin());
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
    Operator->addItems({"<", ">", "<=", ">=", "==", "!=", "different by", "modulo N =="});
    Operator->setCurrentIndex((int)CompareOp::NotEqual);
    CompareTo = new QComboBox(this);
    CompareTo->addItems({"Previous value", "Specific value", "Specific address", "Number of changes"});
    Operand = new QLineEdit(this);
    DifferentBy = new QLineEdit(this);
    DifferentBy->setPlaceholderText("N");
    DifferentBy->setMaximumWidth(100);
    ByLabel = new QLabel("by", this);
    compare->addWidget(new QLabel("Value", this));
    compare->addWidget(Operator);
    compare->addWidget(CompareTo);
    compare->addWidget(Operand, 1);
    compare->addWidget(ByLabel);
    compare->addWidget(DifferentBy);
    layout->addLayout(compare);

    QHBoxLayout* actions = new QHBoxLayout();
    QPushButton* reset = new QPushButton("Reset", this);
    QPushButton* searchButton = new QPushButton("Search", this);
    UndoButton = new QPushButton("Undo", this);
    UndoButton->setEnabled(false);
    AutoSearch = new QCheckBox("Autosearch", this);
    AutoSearch->setToolTip("Apply the filter again on every refresh while the game runs");
    QPushButton* watch = new QPushButton("Add to watch", this);
    QPushButton* cheat = new QPushButton("Add cheat...", this);
    cheat->setToolTip("Make an Action Replay constant write from the selected candidates");
    actions->addWidget(reset);
    actions->addWidget(searchButton);
    actions->addWidget(UndoButton);
    actions->addWidget(AutoSearch);
    actions->addStretch();
    actions->addWidget(watch);
    actions->addWidget(cheat);
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

    QHBoxLayout* edits = new QHBoxLayout();
    QPushButton* eliminate = new QPushButton("Remove selected", this);
    eliminate->setToolTip("Drop the selected candidates from the results (Delete)");
    QPushButton* clearCounts = new QPushButton("Clear changes", this);
    clearCounts->setToolTip("Reset every candidate's change count to 0");
    QPushButton* cheatList = new QPushButton("Cheat list...", this);
    edits->addWidget(eliminate);
    edits->addWidget(clearCounts);
    edits->addStretch();
    edits->addWidget(cheatList);
    layout->addLayout(edits);

    Info = new QLabel(this);
    layout->addWidget(Info);

    QAction* eliminateAction = new QAction(Table);
    eliminateAction->setShortcut(QKeySequence::Delete);
    eliminateAction->setShortcutContext(Qt::WidgetShortcut);
    Table->addAction(eliminateAction);

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
    connect(cheat, &QPushButton::clicked, this, &RamSearchWindow::addCheat);
    connect(AutoSearch, &QCheckBox::toggled, this, &RamSearchWindow::toggleAutoSearch);
    connect(eliminate, &QPushButton::clicked, this, &RamSearchWindow::eliminateSelected);
    connect(eliminateAction, &QAction::triggered, this, &RamSearchWindow::eliminateSelected);
    connect(clearCounts, &QPushButton::clicked, this, &RamSearchWindow::clearChanges);
    connect(cheatList, &QPushButton::clicked, this, [this] { OpenCheatList(Bridge); });
    connect(Table->verticalScrollBar(), &QScrollBar::valueChanged, this, &RamSearchWindow::requestRefresh);
    connect(Table, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.column() != SearchColumn::Value)
            addRowToWatch(index.row());
    });
    connect(bridge, &DebugBridge::symbolsChanged, this, [this] { Model->refreshSymbols(); });

    AutoClock.start();
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
    CompareOp op = (CompareOp)Operator->currentIndex();
    DifferentBy->setEnabled(op == CompareOp::DifferentBy || op == CompareOp::Modulo);
    ByLabel->setText(op == CompareOp::Modulo ? "N" : "by");
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
            SearchFrame = snap.Frame;
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
    runSearch(false);
}

bool RamSearchWindow::readFilter(SearchFilter& filter, u32& reference)
{
    filter.Target = (CompareTarget)CompareTo->currentIndex();
    filter.Op = (CompareOp)Operator->currentIndex();
    bool hex = (RamValueType)Display->currentIndex() == RamValueType::Hex;
    reference = 0;
    switch (filter.Target)
    {
    case CompareTarget::Previous:
        break;
    case CompareTarget::Value:
        if (!ParseInteger(Operand->text(), hex, filter.Operand))
        {
            Info->setText("Enter a value to compare to");
            return false;
        }
        break;
    case CompareTarget::Address:
        if (!Bridge->symbols().parseAddress(SymStr(Operand->text()), reference))
        {
            Info->setText("Enter an address or symbol to compare to");
            return false;
        }
        break;
    case CompareTarget::Changes:
        if (!ParseInteger(Operand->text(), false, filter.Operand))
        {
            Info->setText("Enter a number of changes");
            return false;
        }
        break;
    }
    if (filter.Op == CompareOp::DifferentBy || filter.Op == CompareOp::Modulo)
    {
        if (!ParseInteger(DifferentBy->text(), hex && filter.Target != CompareTarget::Changes, filter.By))
        {
            Info->setText(filter.Op == CompareOp::Modulo ? "Enter the modulus N" : "Enter the difference N");
            return false;
        }
        filter.By = std::llabs(filter.By);
        if (filter.Op == CompareOp::Modulo && filter.By == 0)
        {
            Info->setText("The modulus N cannot be 0");
            return false;
        }
    }
    return true;
}

void RamSearchWindow::runSearch(bool automatic)
{
    if (!Model->started())
    {
        if (!automatic)
            restart();
        return;
    }
    if (!Bridge->hasConsole())
    {
        Info->setText("No game is running");
        return;
    }
    SearchFilter filter;
    u32 reference = 0;
    if (!readFilter(filter, reference))
    {
        if (AutoSearch->isChecked())
            AutoSearch->setChecked(false);
        return;
    }
    RamRegion region = (RamRegion)Region->currentIndex();
    int size = Model->valueSize();
    bool useReference = filter.Target == CompareTarget::Address;
    qulonglong before = Model->count();
    Searching = true;
    AutoClock.restart();
    Bridge->call(this, [region, useReference, reference, size](NDS& nds) {
        RamSnapshot snap = TakeSnapshot(nds, region);
        if (useReference)
            snap.Reference = PeekValue(nds, CPUForAddress(reference), reference, size);
        return snap;
    }, [this, filter, before, automatic](const RamSnapshot& snap) {
        Searching = false;
        if (!Model->sameRegion(snap))
        {
            Model->restart(snap, 1 << Size->currentIndex(), Misaligned->isChecked());
            SearchFrame = snap.Frame;
            showCount("The region moved or changed size, so the search restarted");
            return;
        }
        if (automatic && snap.Frame == SearchFrame)
            return;
        SearchFrame = snap.Frame;
        SearchFilter applied = filter;
        if (applied.Target == CompareTarget::Address)
            applied.Operand = Model->interpret(snap.Reference);
        Model->search(snap, applied, !automatic);
        if (automatic)
        {
            if (Model->count() == 0)
            {
                AutoSearch->setChecked(false);
                showCount("Autosearch stopped: no candidates left");
            }
            else
                showCount(QString("Autosearch at frame %1").arg(snap.Frame));
            return;
        }
        Table->scrollToTop();
        showCount(QString("Was %1").arg(QLocale().toString(before)));
        requestRefresh();
    });
}

void RamSearchWindow::toggleAutoSearch(bool on)
{
    if (on)
        runSearch(false);
}

void RamSearchWindow::undo()
{
    Model->undo();
    showCount("Undone");
    requestRefresh();
}

std::vector<int> RamSearchWindow::selectedRows(size_t cap, size_t& total) const
{
    std::set<int> rows;
    total = 0;
    for (const QItemSelectionRange& range : Table->selectionModel()->selection())
    {
        total += range.height();
        for (int r = range.top(); r <= range.bottom() && rows.size() < cap; r++)
            if ((size_t)r < Model->count())
                rows.insert(r);
    }
    return std::vector<int>(rows.begin(), rows.end());
}

void RamSearchWindow::addRowToWatch(int row)
{
    if (row < 0 || (size_t)row >= Model->count())
        return;
    RamWatchWindow::addWatch(Bridge, parentWidget(), Model->address(row), Model->valueSize(), (RamValueType)Display->currentIndex());
}

void RamSearchWindow::addToWatch()
{
    size_t selected = 0;
    std::vector<int> rows = selectedRows(MaxWatchAdds, selected);
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

void RamSearchWindow::addCheat()
{
    size_t selected = 0;
    std::vector<int> rows = selectedRows(MaxCheatAdds, selected);
    if (rows.empty())
    {
        Info->setText("Select candidates to make cheats from them");
        return;
    }
    std::vector<CheatSource> sources;
    for (int r : rows)
    {
        u32 addr = Model->address(r);
        sources.push_back({addr, Model->valueSize(), CheatName(Bridge, addr, QString())});
    }
    AddCheats(Bridge, this, Info, sources);
}

void RamSearchWindow::eliminateSelected()
{
    std::vector<std::pair<size_t, size_t>> ranges;
    for (const QItemSelectionRange& range : Table->selectionModel()->selection())
        ranges.push_back({(size_t)range.top(), (size_t)range.bottom()});
    if (ranges.empty())
    {
        Info->setText("Select candidates to remove them");
        return;
    }
    std::sort(ranges.begin(), ranges.end());
    qulonglong before = Model->count();
    Model->eliminate(ranges);
    showCount(QString("Removed %1").arg(QLocale().toString(before - (qulonglong)Model->count())));
    requestRefresh();
}

void RamSearchWindow::clearChanges()
{
    Model->clearChanges();
    showCount("Change counts cleared");
}

void RamSearchWindow::refresh()
{
    if (!Model->started())
    {
        if (!Restarting)
            restart();
        return;
    }
    if (AutoSearch->isChecked() && Bridge->isRunning() && AutoClock.hasExpired(AutoSearchMs)
        && (!Searching || AutoClock.hasExpired(StaleSearchMs)))
    {
        runSearch(true);
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
    QPushButton* duplicate = new QPushButton("Duplicate", this);
    QPushButton* separator = new QPushButton("Separator", this);
    QPushButton* cheat = new QPushButton("Add cheat...", this);
    cheat->setToolTip("Make an Action Replay constant write from the selected watches");
    QPushButton* cheatList = new QPushButton("Cheat list...", this);
    buttons->addWidget(remove);
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addWidget(duplicate);
    buttons->addWidget(separator);
    buttons->addStretch();
    buttons->addWidget(cheat);
    buttons->addWidget(cheatList);
    layout->addLayout(buttons);

    QHBoxLayout* files = new QHBoxLayout();
    QPushButton* newButton = new QPushButton("New", this);
    QPushButton* loadButton = new QPushButton("Load...", this);
    QPushButton* appendButton = new QPushButton("Append...", this);
    appendButton->setToolTip("Add the watches of another file to this list");
    QPushButton* recentButton = new QPushButton("Recent", this);
    RecentMenu = new QMenu(recentButton);
    recentButton->setMenu(RecentMenu);
    QPushButton* quickSaveButton = new QPushButton("Save", this);
    quickSaveButton->setToolTip("Save to the file the list came from (Ctrl+S)");
    QPushButton* saveButton = new QPushButton("Save...", this);
    files->addWidget(newButton);
    files->addWidget(loadButton);
    files->addWidget(appendButton);
    files->addWidget(recentButton);
    files->addStretch();
    files->addWidget(quickSaveButton);
    files->addWidget(saveButton);
    layout->addLayout(files);

    Info = new QLabel("Double-click a value to change it in memory.", this);
    layout->addWidget(Info);

    QAction* removeAction = new QAction(Table);
    removeAction->setShortcut(QKeySequence::Delete);
    removeAction->setShortcutContext(Qt::WidgetShortcut);
    Table->addAction(removeAction);

    QAction* saveAction = new QAction(this);
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setShortcutContext(Qt::WindowShortcut);
    addAction(saveAction);

    setAcceptDrops(true);

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
    connect(duplicate, &QPushButton::clicked, this, &RamWatchWindow::duplicateSelected);
    connect(separator, &QPushButton::clicked, this, &RamWatchWindow::addSeparator);
    connect(cheat, &QPushButton::clicked, this, &RamWatchWindow::addCheat);
    connect(cheatList, &QPushButton::clicked, this, [this] { OpenCheatList(Bridge); });
    connect(newButton, &QPushButton::clicked, this, &RamWatchWindow::newList);
    connect(loadButton, &QPushButton::clicked, this, &RamWatchWindow::load);
    connect(appendButton, &QPushButton::clicked, this, &RamWatchWindow::appendFile);
    connect(RecentMenu, &QMenu::aboutToShow, this, &RamWatchWindow::rebuildRecent);
    connect(quickSaveButton, &QPushButton::clicked, this, &RamWatchWindow::quickSave);
    connect(saveAction, &QAction::triggered, this, &RamWatchWindow::quickSave);
    connect(saveButton, &QPushButton::clicked, this, &RamWatchWindow::save);
    connect(Table, &QTableWidget::itemChanged, this, &RamWatchWindow::onItemChanged);
    connect(bridge, &DebugBridge::symbolsChanged, this, [this] {
        for (size_t i = 0; i < Watches.size(); i++)
        {
            resolve(Watches[i]);
            showRow(i);
        }
    });
    connect(bridge, &DebugBridge::consoleChanged, this, &RamWatchWindow::autoLoad);
    connect(bridge, &DebugBridge::started, this, &RamWatchWindow::autoLoad);
    setInterval(100);
    rebuildRecent();
    autoLoad();
    updateTitle();
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
    insertWatch((int)Watches.size(), w);
}

void RamWatchWindow::insertWatch(int row, const Watch& w)
{
    Watches.insert(Watches.begin() + row, w);
    Table->insertRow(row);
    setupRow(row);
    Table->scrollToItem(Table->item(row, WatchColumn::Address));
    markDirty();
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
    if (w.Separator)
    {
        w.Valid = false;
        w.Known = false;
        return;
    }
    u32 old = w.Addr;
    bool wasValid = w.Valid;
    w.Valid = Bridge->symbols().parseAddress(SymStr(w.Expr), w.Addr);
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
    const Watch& w = Watches[row];
    Qt::ItemFlags fixed = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    Qt::ItemFlags editable = QTableWidgetItem().flags();
    QVariant background = w.Separator ? QVariant::fromValue(palette().alternateBase()) : QVariant();
    Filling = true;
    for (int column : {WatchColumn::Address, WatchColumn::Resolved, WatchColumn::Value, WatchColumn::Notes})
    {
        if (!Table->item(row, column))
            Table->setItem(row, column, new QTableWidgetItem());
        Table->item(row, column)->setData(Qt::BackgroundRole, background);
    }
    Table->item(row, WatchColumn::Address)->setFlags(w.Separator ? fixed : editable);
    Table->item(row, WatchColumn::Resolved)->setFlags(fixed);
    Table->item(row, WatchColumn::Value)->setFlags(w.Separator ? fixed : editable);
    Table->item(row, WatchColumn::Notes)->setFlags(editable);
    Filling = false;

    Table->removeCellWidget(row, WatchColumn::Size);
    Table->removeCellWidget(row, WatchColumn::Type);
    if (w.Separator)
    {
        showRow(row);
        return;
    }
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
        markDirty();
        showRow(r);
        requestRefresh();
    });
    connect(type, &QComboBox::currentIndexChanged, this, [this, type](int index) {
        int r = rowOf(type, WatchColumn::Type);
        if (r < 0)
            return;
        Watch& w = Watches[r];
        w.Type = (RamValueType)index;
        markDirty();
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
    QString symbol = w.Valid ? SymStr(Bridge->symbols().describe(w.Addr)) : QString();
    Filling = true;
    Table->item(row, WatchColumn::Address)->setText(w.Expr);
    Table->item(row, WatchColumn::Resolved)->setText(w.Separator ? QString() : !w.Valid ? QString("?") : symbol.isEmpty() ? Hex(w.Addr) : Hex(w.Addr) + "  " + symbol);
    Table->item(row, WatchColumn::Notes)->setText(w.Notes);
    Filling = false;
    QComboBox* size = qobject_cast<QComboBox*>(Table->cellWidget(row, WatchColumn::Size));
    QComboBox* type = qobject_cast<QComboBox*>(Table->cellWidget(row, WatchColumn::Type));
    if (size && type)
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
    Table->item(row, WatchColumn::Value)->setText(w.Separator ? QString() : !w.Valid ? QString("bad address") : w.Known ? FormatValue(w.Raw, w.Size, w.Type) : QString());
    Filling = false;
}

void RamWatchWindow::pokeValue(int row, const QString& text)
{
    Watch& w = Watches[row];
    if (w.Separator)
        return;
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
        markDirty();
        showRow(row);
        requestRefresh();
        break;
    case WatchColumn::Value:
        pokeValue(row, item->text());
        break;
    case WatchColumn::Notes:
        w.Notes = item->text();
        markDirty();
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
    if (!rows.empty())
        markDirty();
}

void RamWatchWindow::moveCurrent(int delta)
{
    int row = Table->currentRow();
    int target = row + delta;
    if (row < 0 || target < 0 || target >= (int)Watches.size())
        return;
    std::swap(Watches[row], Watches[target]);
    setupRow(row);
    setupRow(target);
    Table->selectRow(target);
    markDirty();
}

void RamWatchWindow::duplicateSelected()
{
    int row = Table->currentRow();
    if (row < 0 || row >= (int)Watches.size())
    {
        Info->setText("Select a watch to duplicate");
        return;
    }
    Watch copy = Watches[row];
    copy.Known = false;
    insertWatch(row + 1, copy);
    Table->selectRow(row + 1);
}

void RamWatchWindow::addSeparator()
{
    Watch w;
    w.Separator = true;
    w.Notes = SeparatorNotes;
    int row = Table->currentRow();
    int at = row >= 0 && row < (int)Watches.size() ? row + 1 : (int)Watches.size();
    insertWatch(at, w);
    Table->selectRow(at);
}

void RamWatchWindow::addCheat()
{
    std::vector<int> rows;
    for (const QModelIndex& index : Table->selectionModel()->selectedRows())
        rows.push_back(index.row());
    std::sort(rows.begin(), rows.end());
    std::vector<CheatSource> sources;
    for (int r : rows)
    {
        const Watch& w = Watches[r];
        if (!w.Separator && w.Valid && sources.size() < MaxCheatAdds)
            sources.push_back({w.Addr, w.Size, CheatName(Bridge, w.Addr, w.Notes)});
    }
    if (sources.empty())
    {
        Info->setText("Select watches with a resolved address to make cheats from them");
        return;
    }
    AddCheats(Bridge, this, Info, sources);
}

void RamWatchWindow::markDirty()
{
    if (Dirty)
        return;
    Dirty = true;
    updateTitle();
}

void RamWatchWindow::updateTitle()
{
    QString title = "RAM watch";
    if (!CurrentFile.isEmpty())
        title += " - " + QFileInfo(CurrentFile).fileName();
    if (Dirty && !Watches.empty())
        title += " *";
    setWindowTitle(title);
}

bool RamWatchWindow::askSave()
{
    if (!Dirty || Watches.empty())
        return true;
    QMessageBox::StandardButton answer = QMessageBox::question(this, "melonDS", "Save changes to the watch list?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
    {
        quickSave();
        return !Dirty;
    }
    return true;
}

void RamWatchWindow::newList()
{
    if (!askSave())
        return;
    Table->setRowCount(0);
    Watches.clear();
    CurrentFile.clear();
    Dirty = false;
    updateTitle();
    Info->setText("Started a new watch list");
}

bool RamWatchWindow::writeFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "melonDS", "Cannot write " + path);
        return false;
    }
    QTextStream out(&f);
    for (const Watch& w : Watches)
    {
        QString expr = w.Expr;
        QString notes = w.Notes;
        if (w.Separator)
            out << "-\t0\t" << SeparatorType << '\t' << notes.replace('\t', ' ') << '\n';
        else
            out << expr.replace('\t', ' ') << '\t' << w.Size << '\t' << TypeNames[(int)w.Type] << '\t' << notes.replace('\t', ' ') << '\n';
    }
    CurrentFile = path;
    Dirty = false;
    rememberFile(path);
    updateTitle();
    Info->setText(QString("Saved %1 watches to %2").arg(Watches.size()).arg(path));
    return true;
}

void RamWatchWindow::save()
{
    QString path = QFileDialog::getSaveFileName(this, "Save watches", CurrentFile.isEmpty() ? QString("watches.txt") : CurrentFile, "Text (*.txt)");
    if (path.isEmpty())
        return;
    writeFile(path);
}

void RamWatchWindow::quickSave()
{
    if (CurrentFile.isEmpty())
    {
        save();
        return;
    }
    if (!Dirty)
    {
        Info->setText("No changes since the last save to " + CurrentFile);
        return;
    }
    writeFile(CurrentFile);
}

bool RamWatchWindow::readFile(const QString& path, bool clear)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "melonDS", "Cannot read " + path);
        return false;
    }
    if (clear)
    {
        Table->setRowCount(0);
        Watches.clear();
    }
    QTextStream in(&f);
    int skipped = 0;
    int added = 0;
    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.trimmed().isEmpty())
            continue;
        QStringList fields = line.contains('\t') ? line.split('\t') : line.simplified().split(' ');
        if (fields.size() >= 3 && fields[2].trimmed().compare(QString(SeparatorType), Qt::CaseInsensitive) == 0)
        {
            Watch sep;
            sep.Separator = true;
            sep.Notes = fields.mid(3).join(' ').trimmed();
            insertWatch((int)Watches.size(), sep);
            added++;
            continue;
        }
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
        added++;
    }
    if (clear)
    {
        CurrentFile = path;
        Dirty = false;
        rememberFile(path);
    }
    updateTitle();
    Info->setText(skipped ? QString("Loaded %1 watches, skipped %2 unreadable lines").arg(added).arg(skipped)
                          : QString("Loaded %1 watches").arg(added));
    return true;
}

void RamWatchWindow::load()
{
    if (!askSave())
        return;
    QString path = QFileDialog::getOpenFileName(this, "Load watches", QString(), "Text (*.txt);;All files (*)");
    if (path.isEmpty())
        return;
    readFile(path, true);
}

void RamWatchWindow::appendFile()
{
    QString path = QFileDialog::getOpenFileName(this, "Append watches", QString(), "Text (*.txt);;All files (*)");
    if (path.isEmpty())
        return;
    readFile(path, false);
}

void RamWatchWindow::rememberFile(const QString& path)
{
    Config::Table& cfg = Bridge->instance()->getGlobalConfig();
    QStringList recent = RecentWatchFiles(Bridge);
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > MaxRecentWatchFiles)
        recent.removeLast();
    cfg.SetString(RecentWatchKey, recent.join('\n').toStdString());
    QString code = GameCode(Bridge);
    if (!code.isEmpty())
    {
        cfg.SetString(WatchGameKey + code.toStdString(), path.toStdString());
        LoadedGame = code;
    }
    Config::Save();
}

void RamWatchWindow::rebuildRecent()
{
    RecentMenu->clear();
    QStringList recent = RecentWatchFiles(Bridge);
    for (const QString& path : recent)
        RecentMenu->addAction(path, this, [this, path] {
            if (askSave())
                readFile(path, true);
        });
    if (recent.isEmpty())
        RecentMenu->addAction("No recent watch files")->setEnabled(false);
}

void RamWatchWindow::autoLoad()
{
    QString code = GameCode(Bridge);
    if (code.isEmpty() || code == LoadedGame || (Dirty && !Watches.empty()))
        return;
    LoadedGame = code;
    QString path = QString::fromStdString(Bridge->instance()->getGlobalConfig().GetString(WatchGameKey + code.toStdString()));
    if (path.isEmpty() || path == CurrentFile || !QFile::exists(path))
        return;
    if (readFile(path, true))
        Info->setText(QString("Loaded the %1 watch list %2").arg(code, path));
}

void RamWatchWindow::closeEvent(QCloseEvent* event)
{
    if (!askSave())
    {
        event->ignore();
        return;
    }
    DebugTool::closeEvent(event);
}

void RamWatchWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void RamWatchWindow::dropEvent(QDropEvent* event)
{
    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return;
    QString path = urls.first().toLocalFile();
    event->acceptProposedAction();
    QTimer::singleShot(0, this, [this, path] {
        if (askSave())
            readFile(path, true);
    });
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
