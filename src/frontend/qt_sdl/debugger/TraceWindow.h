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

#ifndef TRACEWINDOW_H
#define TRACEWINDOW_H

#include <vector>

#include "DebugTool.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTreeWidget;

class TraceWindow : public DebugTool
{
    Q_OBJECT

public:
    TraceWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override {}

private:
    QComboBox* CPU;
    QLineEdit* Start;
    QLineEdit* End;
    QSpinBox* Capacity;
    QCheckBox* Active;
    QTableWidget* Table;
    QLabel* Info;
    std::vector<melonDS::TraceEntry> Entries;

    void apply();
    void drain();
    void save();
    void showEntries(const std::vector<melonDS::TraceEntry>& entries);
};

class ProfilerWindow : public DebugTool
{
    Q_OBJECT

public:
    ProfilerWindow(DebugBridge* bridge, QWidget* parent = nullptr);
    ~ProfilerWindow() override;

protected:
    void refresh() override;

private:
    QComboBox* CPU;
    QCheckBox* Calls;
    QCheckBox* Cover;
    QTreeWidget* CallTree;
    QTreeWidget* CoverTree;
    QLabel* Info;
    std::vector<melonDS::u32> CoverAddrs;

    void drainCalls();
    void drainCoverage();
    void saveCoverage();
};

class LastWriterWindow : public DebugTool
{
    Q_OBJECT

public:
    LastWriterWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QCheckBox* Enabled;
    QLineEdit* Addr;
    QSpinBox* Length;
    QTreeWidget* Tree;
    QLabel* Info;
};

#endif
