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

#ifndef RAMSEARCHWINDOW_H
#define RAMSEARCHWINDOW_H

#include <vector>

#include "DebugTool.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QTableView;
class QTableWidget;
class QTableWidgetItem;
class RamSearchModel;
struct SearchFilter;

enum class RamValueType
{
    Signed,
    Unsigned,
    Hex,
    Float,
    Fixed,
};

class RamSearchWindow : public DebugTool
{
    Q_OBJECT

public:
    RamSearchWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QComboBox* Region;
    QComboBox* Size;
    QComboBox* Display;
    QCheckBox* Misaligned;
    QComboBox* CompareTo;
    QLineEdit* Operand;
    QComboBox* Operator;
    QLineEdit* DifferentBy;
    QLabel* ByLabel;
    QPushButton* UndoButton;
    QCheckBox* AutoSearch;
    QTableView* Table;
    RamSearchModel* Model;
    QLabel* Info;
    bool Restarting = false;
    bool Searching = false;
    melonDS::u32 SearchFrame = 0;
    QElapsedTimer AutoClock;

    void restart();
    void search();
    void runSearch(bool automatic);
    bool readFilter(SearchFilter& filter, melonDS::u32& reference);
    void toggleAutoSearch(bool on);
    void undo();
    void addToWatch();
    void addRowToWatch(int row);
    void addCheat();
    void eliminateSelected();
    void clearChanges();
    std::vector<int> selectedRows(size_t cap, size_t& total) const;
    void updateControls();
    void showCount(const QString& prefix);
};

class RamWatchWindow : public DebugTool
{
    Q_OBJECT

public:
    RamWatchWindow(DebugBridge* bridge, QWidget* parent = nullptr);

    static void addWatch(DebugBridge* bridge, QWidget* parent, melonDS::u32 addr, int size, RamValueType type = RamValueType::Hex);
    void append(const QString& expr, int size, RamValueType type, const QString& notes);

protected:
    void refresh() override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct Watch
    {
        QString Expr;
        int Size = 4;
        RamValueType Type = RamValueType::Hex;
        QString Notes;
        bool Separator = false;
        bool Valid = false;
        melonDS::u32 Addr = 0;
        melonDS::u32 Raw = 0;
        bool Known = false;
    };

    QLineEdit* NewAddr;
    QComboBox* NewSize;
    QComboBox* NewType;
    QLineEdit* NewNotes;
    QTableWidget* Table;
    QLabel* Info;
    QMenu* RecentMenu;
    std::vector<Watch> Watches;
    bool Filling = false;
    bool Dirty = false;
    QString CurrentFile;
    QString LoadedGame;

    void addFromInputs();
    void insertWatch(int row, const Watch& w);
    void removeSelected();
    void moveCurrent(int delta);
    void duplicateSelected();
    void addSeparator();
    void addCheat();
    void newList();
    void save();
    void quickSave();
    void load();
    void appendFile();
    bool readFile(const QString& path, bool clear);
    bool writeFile(const QString& path);
    bool askSave();
    void autoLoad();
    void rememberFile(const QString& path);
    void rebuildRecent();
    void markDirty();
    void updateTitle();
    void resolve(Watch& w);
    void setupRow(int row);
    void showRow(int row);
    void showValue(int row);
    void pokeValue(int row, const QString& text);
    void onItemChanged(QTableWidgetItem* item);
    int rowOf(QWidget* widget, int column) const;
    int find(melonDS::u32 addr, int size) const;
};

#endif
