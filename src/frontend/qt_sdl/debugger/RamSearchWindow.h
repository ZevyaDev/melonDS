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
class QPushButton;
class QTableView;
class QTableWidget;
class QTableWidgetItem;
class RamSearchModel;

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
    QPushButton* UndoButton;
    QTableView* Table;
    RamSearchModel* Model;
    QLabel* Info;
    bool Restarting = false;

    void restart();
    void search();
    void undo();
    void addToWatch();
    void addRowToWatch(int row);
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

private:
    struct Watch
    {
        QString Expr;
        int Size = 4;
        RamValueType Type = RamValueType::Hex;
        QString Notes;
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
    std::vector<Watch> Watches;
    bool Filling = false;

    void addFromInputs();
    void removeSelected();
    void moveCurrent(int delta);
    void save();
    void load();
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
