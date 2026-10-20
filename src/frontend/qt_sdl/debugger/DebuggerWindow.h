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

#ifndef DEBUGGERWINDOW_H
#define DEBUGGERWINDOW_H

#include <QMainWindow>

#include <array>
#include <vector>

#include "DebugBridge.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QToolButton;
class QTreeWidget;
class QTableWidget;
class QTimer;
class DisasmView;

struct CPUSnapshot
{
    melonDS::u32 R[16];
    melonDS::u32 CPSR;
    melonDS::u32 SPSR;
    melonDS::u32 PC;
    melonDS::u32 IME, IE, IF;
    bool Halted;
};

struct PointInfo
{
    int ID;
    bool Exec;
    melonDS::u8 CPUMask;
    melonDS::u8 Flags;
    bool Enabled;
    melonDS::u32 Start;
    melonDS::u32 End;
    melonDS::u64 Hits;
    bool Scripted;
    bool Temp;
};

struct DebugSnapshot
{
    std::array<CPUSnapshot, 2> CPU;
    std::vector<PointInfo> Points;
    std::array<std::vector<melonDS::CallFrame>, 2> Stack;
    std::vector<int> Overlays;
    melonDS::u64 Frame;
    bool Debug;
};

class DebuggerWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit DebuggerWindow(DebugBridge* bridge, QWidget* parent = nullptr);
    ~DebuggerWindow() override;

    static DebuggerWindow* open(DebugBridge* bridge, QWidget* parent);
    void goTo(melonDS::u32 addr);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    DebugBridge* Bridge;
    DisasmView* Disasm;
    QComboBox* CPUBox;
    QLineEdit* GotoEdit;
    QLabel* Status;
    QLabel* SymbolStatus;
    std::array<QTableWidget*, 2> Regs;
    QTreeWidget* Points;
    QTreeWidget* Stack;
    QTimer* Timer;
    QSpinBox* StepCount;
    QSpinBox* UpdateEvery;
    QToolButton* DecodeButton;
    std::array<QAction*, 3> DecodeActions {};

    DebugSnapshot Snap {};
    std::array<CPUSnapshot, 2> Prev {};
    bool HavePrev = false;
    bool Pending = false;
    bool FollowPC = false;
    bool FollowLive = false;
    bool Tracking = false;
    int Interval = 250;

    QAction* ActBreak;
    QAction* ActContinue;
    QAction* ActStep;
    QAction* ActOver;
    QAction* ActOut;

    void buildToolbar();
    void buildViewToolbar();
    void buildDocks();
    void setUpdateInterval(int ms);
    void showDecodeMode(int mode);
    void refresh();
    void applySnapshot(const DebugSnapshot& snap);
    void updateActions();
    void updateRegs(int cpu);
    void updatePoints();
    void updateStack();
    void updateStatus();

    int cpu() const;
    void toggleBreakpoint(melonDS::u32 addr);
    void addPointDialog(bool exec);
    void editRegister(int cpu, int row);
    void setPC(melonDS::u32 addr);
    void loadSymbols();
    void autoLoadSymbols();
    QString describeBreak(const melonDS::BreakEvent& ev) const;
};

#endif
