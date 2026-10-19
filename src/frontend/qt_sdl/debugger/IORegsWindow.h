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

#ifndef IOREGSWINDOW_H
#define IOREGSWINDOW_H

#include <vector>

#include "DebugTool.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

class IORegsWindow : public DebugTool
{
    Q_OBJECT

public:
    IORegsWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QComboBox* CPU;
    QLineEdit* Filter;
    QCheckBox* AutoRefresh;
    QTreeWidget* Tree;
    QLabel* Info;
    std::vector<QTreeWidgetItem*> Rows;
    std::vector<melonDS::u64> Previous;
    bool HavePrevious = false;

    void rebuild();
    void applyFilter();
    void showValues(const std::vector<melonDS::u64>& values);
    void editRegister(QTreeWidgetItem* item);
};

#endif
