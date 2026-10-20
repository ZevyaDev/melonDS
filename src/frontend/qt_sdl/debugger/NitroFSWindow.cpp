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
#include <cstring>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "NitroFSWindow.h"
#include "DebugSymbolsQt.h"
#include "DebugTools.h"
#include "NDS.h"

using namespace melonDS;

enum NitroColumn
{
    col_Name,
    col_Id,
    col_Start,
    col_End,
    col_Size,
    col_Ram,
    col_RamSize,
    col_Bss,
    col_InitStart,
    col_InitEnd,
    col_Compressed,
    col_Loaded,
    col_Count,
};

enum NitroKind
{
    kind_Root,
    kind_Dir,
    kind_File,
    kind_Overlay,
};

constexpr int RoleKind = Qt::UserRole;
constexpr int RoleIndex = Qt::UserRole + 1;
constexpr int RolePath = Qt::UserRole + 2;
constexpr u32 OverlayEntrySize = 32;
constexpr u32 QuickViewSize = 256;
constexpr u32 ResidentProbeSize = 64;
constexpr u32 ReadChunk = 1 << 20;
constexpr int MaxDirs = 4096;
constexpr int MaxDirDepth = 64;

static u32 RomU32(const u8* rom, u32 len, u32 off)
{
    u32 value = 0;
    if ((u64)off + 4 <= len)
        memcpy(&value, rom + off, 4);
    return value;
}

static u32 RomU16(const u8* rom, u32 len, u32 off)
{
    u16 value = 0;
    if ((u64)off + 2 <= len)
        memcpy(&value, rom + off, 2);
    return value;
}

static QString FixedString(const char* text, int max)
{
    return QString::fromLatin1(text, (int)strnlen(text, max)).trimmed();
}

static QString DirPath(const std::vector<NitroDir>& dirs, int dir)
{
    QStringList parts;
    for (int depth = 0; dir > 0 && dir < (int)dirs.size() && depth < MaxDirDepth; depth++)
    {
        parts.prepend(dirs[dir].Name);
        dir = dirs[dir].Parent;
    }
    return parts.join('/');
}

static std::vector<char> ComputeResident(NDS& nds, const u8* rom, u32 romLen, const std::vector<NitroOverlay>& overlays)
{
    std::vector<char> resident(overlays.size(), 0);
    for (size_t i = 0; i < overlays.size(); i++)
    {
        const NitroOverlay& o = overlays[i];
        if (o.Compressed || !o.RamSize || o.End <= o.Start || o.End > romLen)
            continue;
        u32 count = std::min<u32>({ResidentProbeSize, o.End - o.Start, o.RamSize}) & ~3u;
        if (count < 4)
            continue;
        bool same = true;
        for (u32 off = 0; off < count && same; off += 4)
            same = nds.Debugger.Peek(o.Cpu, o.Ram + off, 4) == RomU32(rom, romLen, o.Start + off);
        resident[i] = same;
    }
    return resident;
}

static void ParseOverlays(NitroSnapshot& snap, const u8* rom, u32 romLen, int cpu, u32 offset, u32 size, u32 fatOffset, u32 fatCount, std::vector<char>& isOverlay)
{
    u32 count = size / OverlayEntrySize;
    for (u32 i = 0; i < count; i++)
    {
        u32 at = offset + i * OverlayEntrySize;
        if ((u64)at + OverlayEntrySize > romLen)
            break;
        NitroOverlay o {};
        o.Cpu = cpu;
        o.Id = RomU32(rom, romLen, at);
        o.Ram = RomU32(rom, romLen, at + 4);
        o.RamSize = RomU32(rom, romLen, at + 8);
        o.BssSize = RomU32(rom, romLen, at + 12);
        o.InitStart = RomU32(rom, romLen, at + 16);
        o.InitEnd = RomU32(rom, romLen, at + 20);
        o.FileId = RomU32(rom, romLen, at + 24);
        o.Compressed = (RomU32(rom, romLen, at + 28) >> 24) & 1;
        if (o.FileId < fatCount)
        {
            o.Start = RomU32(rom, romLen, fatOffset + o.FileId * 8);
            o.End = RomU32(rom, romLen, fatOffset + o.FileId * 8 + 4);
            isOverlay[o.FileId] = 1;
        }
        o.Name = cpu == 0 ? QString("overlay_%1.bin").arg(o.Id, 4, 10, QChar('0')) : QString("overlay7_%1.bin").arg(o.Id, 4, 10, QChar('0'));
        snap.Overlays.push_back(o);
    }
}

static void ParseFiles(NitroSnapshot& snap, const u8* rom, u32 romLen, const NDSHeader& h, u32 fatCount, const std::vector<char>& isOverlay)
{
    std::vector<char> named(fatCount, 0);
    snap.Dirs.push_back({"data", -1});
    u32 fnt = h.FNTOffset;
    if (h.FNTSize >= 8 && (u64)fnt + 8 <= romLen)
    {
        u32 dirCount = std::min<u32>({RomU16(rom, romLen, fnt + 6), h.FNTSize / 8, (u32)MaxDirs});
        for (u32 i = 1; i < dirCount; i++)
        {
            u32 parent = RomU16(rom, romLen, fnt + i * 8 + 6) & 0xFFF;
            snap.Dirs.push_back({QString("dir_%1").arg(i), parent < dirCount ? (int)parent : 0});
        }
        for (u32 d = 0; d < dirCount; d++)
        {
            u32 pos = fnt + RomU32(rom, romLen, fnt + d * 8);
            u32 fileId = RomU16(rom, romLen, fnt + d * 8 + 4);
            while (pos < romLen)
            {
                u8 len = rom[pos++];
                if (len == 0 || len == 0x80)
                    break;
                u32 n = len & 0x7F;
                if ((u64)pos + n > romLen)
                    break;
                QString name = QString::fromLatin1((const char*)rom + pos, n);
                pos += n;
                if (len & 0x80)
                {
                    u32 child = RomU16(rom, romLen, pos) & 0xFFF;
                    pos += 2;
                    if (child > 0 && child < dirCount)
                        snap.Dirs[child].Name = name;
                    continue;
                }
                u32 id = fileId++;
                if (id >= fatCount)
                    continue;
                named[id] = 1;
                if (isOverlay[id])
                    continue;
                NitroFile f {};
                f.Id = id;
                f.Start = RomU32(rom, romLen, h.FATOffset + id * 8);
                f.End = RomU32(rom, romLen, h.FATOffset + id * 8 + 4);
                f.Name = name;
                f.Dir = (int)d;
                snap.Files.push_back(f);
            }
        }
    }
    for (u32 id = 0; id < fatCount; id++)
    {
        if (named[id] || isOverlay[id])
            continue;
        NitroFile f {};
        f.Id = id;
        f.Start = RomU32(rom, romLen, h.FATOffset + id * 8);
        f.End = RomU32(rom, romLen, h.FATOffset + id * 8 + 4);
        f.Name = QString("file_%1.bin").arg(id, 4, 10, QChar('0'));
        f.Dir = -1;
        snap.Files.push_back(f);
    }
    for (NitroFile& f : snap.Files)
    {
        QString dir = f.Dir < 0 ? QString("unnamed") : DirPath(snap.Dirs, f.Dir);
        f.Path = dir.isEmpty() ? f.Name : dir + "/" + f.Name;
    }
}

static NitroSnapshot ReadCart(NDS& nds, bool haveTree, const QString& knownCode, u32 knownLength, const std::vector<NitroOverlay>& knownOverlays)
{
    NitroSnapshot snap;
    const NDSCart::CartCommon* cart = nds.GetNDSCart();
    if (!cart || !cart->GetROM())
    {
        snap.Message = "No cartridge loaded";
        return snap;
    }
    snap.HasCart = true;
    const u8* rom = cart->GetROM();
    u32 romLen = cart->GetROMLength();
    const NDSHeader& h = cart->GetHeader();
    snap.Code = QString::fromLatin1(h.GameCode, 4);
    snap.RomLength = romLen;

    if (haveTree && snap.Code == knownCode && romLen == knownLength)
    {
        snap.Resident = ComputeResident(nds, rom, romLen, knownOverlays);
        return snap;
    }

    snap.Reparsed = true;
    NitroHeader& hd = snap.Header;
    hd.Title = FixedString(h.GameTitle, 12);
    hd.Code = snap.Code;
    hd.Arm9Rom = h.ARM9ROMOffset;
    hd.Arm9Entry = h.ARM9EntryAddress;
    hd.Arm9Ram = h.ARM9RAMAddress;
    hd.Arm9Size = h.ARM9Size;
    hd.Arm7Rom = h.ARM7ROMOffset;
    hd.Arm7Entry = h.ARM7EntryAddress;
    hd.Arm7Ram = h.ARM7RAMAddress;
    hd.Arm7Size = h.ARM7Size;
    hd.FntOffset = h.FNTOffset;
    hd.FntSize = h.FNTSize;
    hd.FatOffset = h.FATOffset;
    hd.FatSize = h.FATSize;
    hd.Ovl9Offset = h.ARM9OverlayOffset;
    hd.Ovl9Size = h.ARM9OverlaySize;
    hd.Ovl7Offset = h.ARM7OverlayOffset;
    hd.Ovl7Size = h.ARM7OverlaySize;
    hd.BannerOffset = h.BannerOffset;
    hd.UsedSize = h.ROMSize;

    bool fatValid = h.FATSize >= 8 && (u64)h.FATOffset + h.FATSize <= romLen;
    u32 fatCount = fatValid ? h.FATSize / 8 : 0;
    if (!fatCount)
    {
        snap.Message = "No file system in this ROM (homebrew or no FAT)";
        return snap;
    }
    std::vector<char> isOverlay(fatCount, 0);
    ParseOverlays(snap, rom, romLen, 0, h.ARM9OverlayOffset, h.ARM9OverlaySize, h.FATOffset, fatCount, isOverlay);
    ParseOverlays(snap, rom, romLen, 1, h.ARM7OverlayOffset, h.ARM7OverlaySize, h.FATOffset, fatCount, isOverlay);
    ParseFiles(snap, rom, romLen, h, fatCount, isOverlay);
    if (h.FNTSize < 8)
        snap.Message = "No FNT: files are listed by id";
    snap.Resident = ComputeResident(nds, rom, romLen, snap.Overlays);
    return snap;
}

static QString SafeName(QString name)
{
    for (QChar& c : name)
        if (QString("<>:\"/\\|?*").contains(c) || c.unicode() < 32)
            c = '_';
    if (name.isEmpty() || name == "." || name == "..")
        name = "_";
    return name;
}

static QString HexDump(u32 start, const QByteArray& data)
{
    QString out;
    for (int i = 0; i < data.size(); i += 16)
    {
        QString hex, ascii;
        for (int j = 0; j < 16; j++)
        {
            if (i + j < data.size())
            {
                u8 b = (u8)data[i + j];
                hex += Hex(b, 2) + " ";
                ascii += (b >= 0x20 && b < 0x7F) ? QChar(b) : QChar('.');
            }
            else
                hex += "   ";
        }
        out += Hex(start + i) + "  " + hex + " " + ascii + "\n";
    }
    return out;
}

NitroFSWindow::NitroFSWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "NitroFS", parent)
{
    resize(1250, 800);
    QVBoxLayout* layout = new QVBoxLayout(this);

    HeaderLabel = new QLabel(this);
    HeaderLabel->setObjectName("header");
    HeaderLabel->setFont(MonoFont());
    HeaderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(HeaderLabel);

    QHBoxLayout* top = new QHBoxLayout();
    Filter = new QLineEdit(this);
    Filter->setObjectName("filter");
    Filter->setPlaceholderText("Filter by path");
    Filter->setClearButtonEnabled(true);
    top->addWidget(Filter, 1);
    ViewButton = new QPushButton("View in memory", this);
    ExtractButton = new QPushButton("Extract file...", this);
    ExtractAllButton = new QPushButton("Extract all...", this);
    DisasmButton = new QPushButton("Disassemble at RAM", this);
    MemoryButton = new QPushButton("Memory at RAM", this);
    ViewButton->setObjectName("view");
    ExtractButton->setObjectName("extract");
    ExtractAllButton->setObjectName("extractAll");
    DisasmButton->setObjectName("disassemble");
    MemoryButton->setObjectName("memory");
    for (QPushButton* b : {ViewButton, ExtractButton, ExtractAllButton, DisasmButton, MemoryButton})
        top->addWidget(b);
    layout->addLayout(top);

    Tree = new QTreeWidget(this);
    Tree->setObjectName("tree");
    Tree->setColumnCount(col_Count);
    Tree->setHeaderLabels({"Name", "ID", "ROM start", "ROM end", "Size", "RAM", "RAM size", "BSS size", "Init start", "Init end", "Compressed", "Loaded"});
    Tree->setFont(MonoFont());
    Tree->setUniformRowHeights(true);
    Tree->setContextMenuPolicy(Qt::CustomContextMenu);
    Tree->header()->setStretchLastSection(false);
    Tree->setColumnWidth(col_Name, 360);
    layout->addWidget(Tree, 1);

    Info = new QLabel(this);
    Info->setObjectName("info");
    Info->setFont(MonoFont());
    Info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(Info);

    Quick = new QPlainTextEdit(this);
    Quick->setObjectName("quick");
    Quick->setReadOnly(true);
    Quick->setFont(MonoFont());
    Quick->setLineWrapMode(QPlainTextEdit::NoWrap);
    Quick->setMaximumHeight(QFontMetrics(MonoFont()).lineSpacing() * 18);
    layout->addWidget(Quick);

    Status = new QLabel(this);
    Status->setObjectName("status");
    layout->addWidget(Status);

    connect(Filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(Tree, &QTreeWidget::currentItemChanged, this, [this] { selectionChanged(); });
    connect(Tree, &QTreeWidget::itemDoubleClicked, this, [this] { viewInMemory(); });
    connect(Tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showMenu(pos); });
    connect(ViewButton, &QPushButton::clicked, this, [this] { viewInMemory(); });
    connect(ExtractButton, &QPushButton::clicked, this, [this] { extractSelected(); });
    connect(ExtractAllButton, &QPushButton::clicked, this, [this] { extractAll(); });
    connect(DisasmButton, &QPushButton::clicked, this, [this] { disassembleOverlay(); });
    connect(MemoryButton, &QPushButton::clicked, this, [this] { memoryOverlay(); });

    updateButtons();
    setInterval(1000);
}

void NitroFSWindow::refresh()
{
    bool have = HaveTree;
    QString code = CurCode;
    u32 length = CurLength;
    std::vector<NitroOverlay> overlays = Overlays;
    std::shared_ptr<const DebugSymbols::Set> set = Bridge->symbols().set();
    fetch([have, code, length, overlays, set](NDS& nds) {
              NitroSnapshot snap = ReadCart(nds, have, code, length, overlays);
              if (set && snap.HasCart)
              {
                  snap.SymbolLoaded = DebugSymbols::detect(*set, nds);
                  snap.HasSymbolLoaded = true;
              }
              return snap;
          },
          [this](const NitroSnapshot& snap) { showSnapshot(snap); });
}

void NitroFSWindow::showSnapshot(const NitroSnapshot& snap)
{
    if (snap.HasSymbolLoaded)
        Bridge->symbols().setLoaded(snap.SymbolLoaded);
    if (!snap.HasCart)
    {
        if (HaveTree || Tree->topLevelItemCount())
        {
            HaveTree = false;
            CurCode.clear();
            CurLength = 0;
            Dirs.clear();
            Files.clear();
            Overlays.clear();
            OverlayItems.clear();
            Resident.clear();
            Tree->clear();
            HeaderLabel->clear();
            Info->clear();
            Quick->clear();
            updateButtons();
        }
        Status->setText(snap.Message);
        return;
    }

    if (snap.Reparsed)
    {
        HaveTree = true;
        CurCode = snap.Code;
        CurLength = snap.RomLength;
        CurHeader = snap.Header;
        Dirs = snap.Dirs;
        Files = snap.Files;
        Overlays = snap.Overlays;
        Resident = snap.Resident;

        const NitroHeader& h = CurHeader;
        HeaderLabel->setText(QString("%1  [%2]  ROM size %3  used %20  icon/title %21\n"
                                     "ARM9: rom %4  entry %5  ram %6  size %7    ARM7: rom %8  entry %9  ram %10  size %11\n"
                                     "FNT: %12 (%13)  FAT: %14 (%15)  ARM9 ovt: %16 (%17)  ARM7 ovt: %18 (%19)")
                                 .arg(h.Title, h.Code).arg(snap.RomLength)
                                 .arg(Hex(h.Arm9Rom), Hex(h.Arm9Entry), Hex(h.Arm9Ram), Hex(h.Arm9Size))
                                 .arg(Hex(h.Arm7Rom), Hex(h.Arm7Entry), Hex(h.Arm7Ram), Hex(h.Arm7Size))
                                 .arg(Hex(h.FntOffset), Hex(h.FntSize), Hex(h.FatOffset), Hex(h.FatSize))
                                 .arg(Hex(h.Ovl9Offset), Hex(h.Ovl9Size), Hex(h.Ovl7Offset), Hex(h.Ovl7Size))
                                 .arg(Hex(h.UsedSize), Hex(h.BannerOffset)));
        Status->setText(snap.Message.isEmpty() ? QString("%1 files, %2 overlays").arg(Files.size()).arg(Overlays.size()) : snap.Message);
        rebuildTree();
        return;
    }

    Resident = snap.Resident;
    updateLoaded();
}

void NitroFSWindow::rebuildTree()
{
    QString selected;
    if (QTreeWidgetItem* cur = Tree->currentItem())
        selected = cur->data(0, RolePath).toString();

    Tree->setUpdatesEnabled(false);
    Tree->clear();
    OverlayItems.clear();

    auto makeItem = [](int kind, int index, const QString& name, const QString& path) {
        QTreeWidgetItem* item = new QTreeWidgetItem();
        item->setText(col_Name, name);
        item->setData(0, RoleKind, kind);
        item->setData(0, RoleIndex, index);
        item->setData(0, RolePath, path);
        return item;
    };

    QTreeWidgetItem* dataRoot = makeItem(kind_Root, 0, "data", "data");
    QTreeWidgetItem* overlayRoot = makeItem(kind_Root, 1, "overlay", "overlay");
    Tree->addTopLevelItem(dataRoot);
    Tree->addTopLevelItem(overlayRoot);

    std::vector<QTreeWidgetItem*> dirItems(Dirs.size(), nullptr);
    for (size_t i = 1; i < Dirs.size(); i++)
        dirItems[i] = makeItem(kind_Dir, (int)i, Dirs[i].Name, "data/" + DirPath(Dirs, (int)i));
    if (!dirItems.empty())
        dirItems[0] = dataRoot;
    for (size_t i = 1; i < Dirs.size(); i++)
    {
        int parent = Dirs[i].Parent;
        QTreeWidgetItem* host = parent >= 0 && parent < (int)dirItems.size() && parent != (int)i ? dirItems[parent] : dataRoot;
        host->addChild(dirItems[i]);
    }

    QTreeWidgetItem* unnamed = nullptr;
    for (size_t i = 0; i < Files.size(); i++)
    {
        const NitroFile& f = Files[i];
        QTreeWidgetItem* item = makeItem(kind_File, (int)i, f.Name, "data/" + f.Path);
        item->setText(col_Id, QString::number(f.Id));
        item->setText(col_Start, Hex(f.Start));
        item->setText(col_End, Hex(f.End));
        item->setText(col_Size, QString::number(f.End >= f.Start ? f.End - f.Start : 0));
        QTreeWidgetItem* host = dataRoot;
        if (f.Dir < 0)
        {
            if (!unnamed)
            {
                unnamed = makeItem(kind_Dir, 0, "unnamed", "data/unnamed");
                dataRoot->addChild(unnamed);
            }
            host = unnamed;
        }
        else if (f.Dir < (int)dirItems.size() && dirItems[f.Dir])
            host = dirItems[f.Dir];
        host->addChild(item);
    }

    for (size_t i = 0; i < Overlays.size(); i++)
    {
        const NitroOverlay& o = Overlays[i];
        QTreeWidgetItem* item = makeItem(kind_Overlay, (int)i, o.Name, "overlay/" + o.Name);
        item->setText(col_Id, QString::number(o.FileId));
        item->setText(col_Start, Hex(o.Start));
        item->setText(col_End, Hex(o.End));
        item->setText(col_Size, QString::number(o.End >= o.Start ? o.End - o.Start : 0));
        item->setText(col_Ram, Hex(o.Ram));
        item->setText(col_RamSize, Hex(o.RamSize));
        item->setText(col_Bss, Hex(o.BssSize));
        item->setText(col_InitStart, Hex(o.InitStart));
        item->setText(col_InitEnd, Hex(o.InitEnd));
        item->setText(col_Compressed, o.Compressed ? "yes" : "no");
        overlayRoot->addChild(item);
        OverlayItems.push_back(item);
    }

    dataRoot->setExpanded(true);
    overlayRoot->setExpanded(true);
    for (int c = col_Id; c < col_Count; c++)
        Tree->resizeColumnToContents(c);

    updateLoaded();
    applyFilter();

    if (!selected.isEmpty())
    {
        QTreeWidgetItemIterator it(Tree);
        while (*it)
        {
            if ((*it)->data(0, RolePath).toString() == selected)
            {
                Tree->setCurrentItem(*it);
                break;
            }
            ++it;
        }
    }
    Tree->setUpdatesEnabled(true);
    selectionChanged();
}

void NitroFSWindow::updateLoaded()
{
    const DebugSymbols& symbols = Bridge->symbols();
    bool useSymbols = !symbols.empty();
    const std::vector<int>& loaded = symbols.loaded();
    bool changed = false;
    for (size_t i = 0; i < OverlayItems.size() && i < Overlays.size(); i++)
    {
        const NitroOverlay& o = Overlays[i];
        bool byName = useSymbols && o.Cpu == 0 && std::find(loaded.begin(), loaded.end(), (int)o.Id) != loaded.end();
        bool byBytes = i < Resident.size() && Resident[i];
        QString text = (byName || byBytes) ? "loaded" : (o.Compressed ? "compressed" : "not loaded");
        if (OverlayItems[i]->text(col_Loaded) != text)
        {
            OverlayItems[i]->setText(col_Loaded, text);
            changed = true;
        }
    }
    if (changed)
        Tree->resizeColumnToContents(col_Loaded);
}

bool NitroFSWindow::applyFilterTo(QTreeWidgetItem* item, const QString& needle, bool parentMatched)
{
    bool matched = needle.isEmpty() || parentMatched || item->data(0, RolePath).toString().toLower().contains(needle);
    int kind = item->data(0, RoleKind).toInt();
    bool visible = matched;
    if (kind == kind_Root || kind == kind_Dir)
    {
        bool any = false;
        for (int i = 0; i < item->childCount(); i++)
            any |= applyFilterTo(item->child(i), needle, matched);
        visible = matched || any;
        if (!needle.isEmpty() && visible)
            item->setExpanded(true);
    }
    item->setHidden(!visible);
    return visible;
}

void NitroFSWindow::applyFilter()
{
    QString needle = Filter->text().trimmed().toLower();
    for (int i = 0; i < Tree->topLevelItemCount(); i++)
        applyFilterTo(Tree->topLevelItem(i), needle, false);
}

bool NitroFSWindow::selectedFile(const NitroFile*& file) const
{
    QTreeWidgetItem* item = Tree->currentItem();
    if (!item || item->data(0, RoleKind).toInt() != kind_File)
        return false;
    int index = item->data(0, RoleIndex).toInt();
    if (index < 0 || index >= (int)Files.size())
        return false;
    file = &Files[index];
    return true;
}

bool NitroFSWindow::selectedOverlay(const NitroOverlay*& overlay) const
{
    QTreeWidgetItem* item = Tree->currentItem();
    if (!item || item->data(0, RoleKind).toInt() != kind_Overlay)
        return false;
    int index = item->data(0, RoleIndex).toInt();
    if (index < 0 || index >= (int)Overlays.size())
        return false;
    overlay = &Overlays[index];
    return true;
}

bool NitroFSWindow::selectedRange(u32& start, u32& end) const
{
    const NitroFile* file = nullptr;
    const NitroOverlay* overlay = nullptr;
    if (selectedFile(file))
    {
        start = file->Start;
        end = file->End;
        return true;
    }
    if (selectedOverlay(overlay))
    {
        start = overlay->Start;
        end = overlay->End;
        return true;
    }
    return false;
}

void NitroFSWindow::updateButtons()
{
    u32 start, end;
    bool range = selectedRange(start, end);
    const NitroOverlay* overlay = nullptr;
    bool isOverlay = selectedOverlay(overlay);
    ViewButton->setEnabled(range);
    ExtractButton->setEnabled(range);
    ExtractAllButton->setEnabled(HaveTree);
    DisasmButton->setEnabled(isOverlay);
    MemoryButton->setEnabled(isOverlay);
}

void NitroFSWindow::selectionChanged()
{
    updateButtons();
    u32 start, end;
    QTreeWidgetItem* item = Tree->currentItem();
    if (!item)
    {
        Info->clear();
        Quick->clear();
        return;
    }
    if (!selectedRange(start, end))
    {
        Info->setText(item->data(0, RolePath).toString());
        Quick->clear();
        return;
    }
    u32 size = end >= start ? end - start : 0;
    Info->setText(QString("%1  [%2-%3] size %4").arg(item->data(0, RolePath).toString(), Hex(start), Hex(end)).arg(size));
    u32 count = std::min(size, QuickViewSize);
    if (!count)
    {
        Quick->clear();
        return;
    }
    Bridge->call(this, [start, count](NDS& nds) {
        QByteArray data;
        const NDSCart::CartCommon* cart = nds.GetNDSCart();
        if (cart && cart->GetROM() && (u64)start + count <= cart->GetROMLength())
            data = QByteArray((const char*)cart->GetROM() + start, (qsizetype)count);
        return data;
    }, [this, start](const QByteArray& data) {
        u32 now, end;
        if (selectedRange(now, end) && now == start)
            Quick->setPlainText(HexDump(start, data));
    });
}

bool NitroFSWindow::readRom(u32 start, u32 size, QByteArray& out)
{
    out.clear();
    out.reserve((qsizetype)size);
    QString code = CurCode;
    for (u32 done = 0; done < size;)
    {
        u32 n = std::min(ReadChunk, size - done);
        u32 at = start + done;
        QByteArray chunk;
        bool ok = false;
        Bridge->run([&](NDS& nds) {
            const NDSCart::CartCommon* cart = nds.GetNDSCart();
            if (!cart || !cart->GetROM() || QString::fromLatin1(cart->GetHeader().GameCode, 4) != code || (u64)at + n > cart->GetROMLength())
                return;
            chunk = QByteArray((const char*)cart->GetROM() + at, (qsizetype)n);
            ok = true;
        }, 5000);
        if (!ok)
            return false;
        out.append(chunk);
        done += n;
    }
    return true;
}

void NitroFSWindow::viewInMemory()
{
    u32 start, end;
    if (selectedRange(start, end))
        DebugTools::showMemory(Bridge, this, DebugTools::memSpace_CartROM, start);
}

void NitroFSWindow::extractSelected()
{
    u32 start, end;
    if (!selectedRange(start, end))
        return;
    const NitroFile* file = nullptr;
    const NitroOverlay* overlay = nullptr;
    QString name = selectedFile(file) ? file->Name : (selectedOverlay(overlay) ? overlay->Name : QString("file.bin"));
    QString path = QFileDialog::getSaveFileName(this, "Extract file", SafeName(name));
    if (path.isEmpty())
        return;
    QByteArray data;
    if (end < start || !readRom(start, end - start, data))
    {
        QMessageBox::warning(this, "NitroFS", "Could not read the file from the cartridge.");
        return;
    }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
    {
        QMessageBox::warning(this, "NitroFS", "Could not write " + path);
        return;
    }
    Status->setText(QString("Extracted %1 (%2 bytes)").arg(path).arg(data.size()));
}

void NitroFSWindow::extractAll()
{
    if (!HaveTree)
        return;
    QString root = QFileDialog::getExistingDirectory(this, "Extract all to");
    if (root.isEmpty())
        return;

    int total = (int)(Files.size() + Overlays.size());
    QProgressDialog progress("Extracting...", "Cancel", 0, total, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    int done = 0;
    int failed = 0;
    auto extract = [&](const QString& relative, u32 start, u32 end) {
        QStringList parts = relative.split('/');
        for (QString& p : parts)
            p = SafeName(p);
        QString path = QDir(root).filePath(parts.join('/'));
        QByteArray data;
        bool ok = end >= start && QDir().mkpath(QFileInfo(path).absolutePath()) && readRom(start, end - start, data);
        if (ok)
        {
            QFile out(path);
            ok = out.open(QIODevice::WriteOnly) && out.write(data) == data.size();
        }
        if (!ok)
            failed++;
    };

    for (const NitroFile& f : Files)
    {
        progress.setValue(done++);
        progress.setLabelText(f.Path);
        QApplication::processEvents();
        if (progress.wasCanceled())
            break;
        extract(f.Path, f.Start, f.End);
    }
    for (const NitroOverlay& o : Overlays)
    {
        if (progress.wasCanceled())
            break;
        progress.setValue(done++);
        progress.setLabelText("overlay/" + o.Name);
        QApplication::processEvents();
        extract("overlay/" + o.Name, o.Start, o.End);
    }
    bool canceled = progress.wasCanceled();
    progress.setValue(total);
    Status->setText(QString("%1 %2 of %3 files to %4, %5 failed").arg(canceled ? "Canceled after" : "Extracted").arg(done).arg(total).arg(root).arg(failed));
}

void NitroFSWindow::disassembleOverlay()
{
    const NitroOverlay* overlay = nullptr;
    if (selectedOverlay(overlay))
        DebugTools::showDisassembly(Bridge, this, overlay->Ram);
}

void NitroFSWindow::memoryOverlay()
{
    const NitroOverlay* overlay = nullptr;
    if (selectedOverlay(overlay))
        DebugTools::showMemory(Bridge, this, overlay->Cpu == 0 ? DebugTools::memSpace_ARM9 : DebugTools::memSpace_ARM7, overlay->Ram);
}

void NitroFSWindow::showMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = Tree->itemAt(pos);
    if (item)
        Tree->setCurrentItem(item);
    updateButtons();
    QMenu menu(this);
    QAction* view = menu.addAction("View in memory viewer");
    view->setEnabled(ViewButton->isEnabled());
    QAction* extract = menu.addAction("Extract file...");
    extract->setEnabled(ExtractButton->isEnabled());
    QAction* all = menu.addAction("Extract all...");
    all->setEnabled(ExtractAllButton->isEnabled());
    menu.addSeparator();
    QAction* disasm = menu.addAction("Disassemble at RAM address");
    disasm->setEnabled(DisasmButton->isEnabled());
    QAction* memory = menu.addAction("Memory at RAM address");
    memory->setEnabled(MemoryButton->isEnabled());
    QAction* chosen = menu.exec(Tree->viewport()->mapToGlobal(pos));
    if (chosen == view)
        viewInMemory();
    else if (chosen == extract)
        extractSelected();
    else if (chosen == all)
        extractAll();
    else if (chosen == disasm)
        disassembleOverlay();
    else if (chosen == memory)
        memoryOverlay();
}
