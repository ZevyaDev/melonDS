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

#ifndef NITROFSWINDOW_H
#define NITROFSWINDOW_H

#include <vector>

#include <QString>

#include "DebugTool.h"

class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

struct NitroDir
{
    QString Name;
    int Parent;
};

struct NitroFile
{
    melonDS::u32 Id;
    melonDS::u32 Start;
    melonDS::u32 End;
    QString Name;
    QString Path;
    int Dir;
};

struct NitroOverlay
{
    int Cpu;
    melonDS::u32 Id;
    melonDS::u32 Ram;
    melonDS::u32 RamSize;
    melonDS::u32 BssSize;
    melonDS::u32 InitStart;
    melonDS::u32 InitEnd;
    melonDS::u32 FileId;
    bool Compressed;
    melonDS::u32 Start;
    melonDS::u32 End;
    QString Name;
};

struct NitroHeader
{
    QString Title;
    QString Code;
    melonDS::u32 Arm9Rom, Arm9Entry, Arm9Ram, Arm9Size;
    melonDS::u32 Arm7Rom, Arm7Entry, Arm7Ram, Arm7Size;
    melonDS::u32 FntOffset, FntSize, FatOffset, FatSize;
    melonDS::u32 Ovl9Offset, Ovl9Size, Ovl7Offset, Ovl7Size;
    melonDS::u32 BannerOffset, UsedSize;
};

struct NitroSnapshot
{
    bool HasCart = false;
    bool Reparsed = false;
    QString Message;
    QString Code;
    melonDS::u32 RomLength = 0;
    NitroHeader Header {};
    std::vector<NitroDir> Dirs;
    std::vector<NitroFile> Files;
    std::vector<NitroOverlay> Overlays;
    std::vector<char> Resident;
    bool HasSymbolLoaded = false;
    std::vector<int> SymbolLoaded;
};

class NitroFSWindow : public DebugTool
{
    Q_OBJECT

public:
    NitroFSWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QLabel* HeaderLabel;
    QLabel* Status;
    QLineEdit* Filter;
    QTreeWidget* Tree;
    QLabel* Info;
    QPlainTextEdit* Quick;
    QPushButton* ViewButton;
    QPushButton* ExtractButton;
    QPushButton* ExtractAllButton;
    QPushButton* DisasmButton;
    QPushButton* MemoryButton;

    bool HaveTree = false;
    QString CurCode;
    melonDS::u32 CurLength = 0;
    NitroHeader CurHeader {};
    std::vector<NitroDir> Dirs;
    std::vector<NitroFile> Files;
    std::vector<NitroOverlay> Overlays;
    std::vector<QTreeWidgetItem*> OverlayItems;
    std::vector<char> Resident;

    void showSnapshot(const NitroSnapshot& snap);
    void rebuildTree();
    void updateLoaded();
    void applyFilter();
    bool applyFilterTo(QTreeWidgetItem* item, const QString& needle, bool parentMatched);
    void selectionChanged();
    void updateButtons();
    void showMenu(const QPoint& pos);

    bool selectedFile(const NitroFile*& file) const;
    bool selectedOverlay(const NitroOverlay*& overlay) const;
    bool selectedRange(melonDS::u32& start, melonDS::u32& end) const;
    bool readRom(melonDS::u32 start, melonDS::u32 size, QByteArray& out);

    void viewInMemory();
    void extractSelected();
    void extractAll();
    void disassembleOverlay();
    void memoryOverlay();
};

#endif
