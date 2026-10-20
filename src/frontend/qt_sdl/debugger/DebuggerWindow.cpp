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

#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "DebuggerWindow.h"
#include "DebugSymbolsQt.h"
#include "DebugTool.h"
#include "DebugTools.h"
#include "DisasmView.h"
#include "EmuInstance.h"
#include "Config.h"
#include "ARM.h"
#include "NDSCart.h"

using namespace melonDS;

static QMap<DebugBridge*, DebuggerWindow*> OpenWindows;

constexpr int MaxStepCount = 1000000;
constexpr int MaxUpdateMs = 10000;
constexpr int UpdateStepMs = 50;

static const char* const RegNames[16] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
};

static QString ModeName(u32 mode)
{
    switch (mode & 0x1F)
    {
    case 0x10: return "USR";
    case 0x11: return "FIQ";
    case 0x12: return "IRQ";
    case 0x13: return "SVC";
    case 0x17: return "ABT";
    case 0x1B: return "UND";
    case 0x1F: return "SYS";
    default: return "???";
    }
}

static QString Flags(u32 cpsr)
{
    QString s;
    s += (cpsr & (1u << 31)) ? 'N' : '-';
    s += (cpsr & (1u << 30)) ? 'Z' : '-';
    s += (cpsr & (1u << 29)) ? 'C' : '-';
    s += (cpsr & (1u << 28)) ? 'V' : '-';
    s += (cpsr & (1u << 27)) ? 'Q' : '-';
    s += ' ';
    s += (cpsr & 0x80) ? 'I' : '-';
    s += (cpsr & 0x40) ? 'F' : '-';
    s += (cpsr & 0x20) ? 'T' : '-';
    return s + " " + ModeName(cpsr);
}

static u32 SPSRFor(ARM& arm)
{
    switch (arm.CPSR & 0x1F)
    {
    case 0x11: return arm.R_FIQ[7];
    case 0x12: return arm.R_IRQ[2];
    case 0x13: return arm.R_SVC[2];
    case 0x17: return arm.R_ABT[2];
    case 0x1B: return arm.R_UND[2];
    default: return 0;
    }
}

DebuggerWindow* DebuggerWindow::open(DebugBridge* bridge, QWidget* parent)
{
    DebuggerWindow* win = OpenWindows.value(bridge);
    if (!win)
    {
        win = new DebuggerWindow(bridge, parent);
        OpenWindows.insert(bridge, win);
    }
    win->show();
    win->raise();
    win->activateWindow();
    return win;
}

DebuggerWindow::DebuggerWindow(DebugBridge* bridge, QWidget* parent) : QMainWindow(parent, Qt::Window), Bridge(bridge)
{
    setAttribute(Qt::WA_DeleteOnClose);
    bridge->adopt(this);
    setWindowTitle(QString("Debugger - instance %1").arg(bridge->instance()->getInstanceID() + 1));
    resize(1100, 700);

    Disasm = new DisasmView(bridge, this);
    setCentralWidget(Disasm);
    connect(Disasm, &DisasmView::toggleBreakpoint, this, &DebuggerWindow::toggleBreakpoint);
    connect(Disasm, &DisasmView::runToCursor, this, [this](u32 addr) { Bridge->runTo(cpu(), addr); });
    connect(Disasm, &DisasmView::setPCRequested, this, &DebuggerWindow::setPC);

    buildToolbar();
    buildViewToolbar();
    buildDocks();

    Status = new QLabel(this);
    SymbolStatus = new QLabel(this);
    statusBar()->addWidget(Status, 1);
    statusBar()->addPermanentWidget(SymbolStatus);

    Timer = new QTimer(this);
    connect(Timer, &QTimer::timeout, this, [this] {
        if (Bridge->isRunning())
        {
            refresh();
            Disasm->reload();
        }
    });

    connect(bridge, &DebugBridge::broke, this, [this](BreakEvent ev) {
        FollowPC = true;
        if (ev.CPU != cpu())
            CPUBox->setCurrentIndex(ev.CPU);
        refresh();
    });
    connect(bridge, &DebugBridge::resumed, this, [this] {
        updateActions();
        updateStatus();
    });
    connect(bridge, &DebugBridge::pauseChanged, this, [this](bool paused) {
        FollowPC = paused;
        refresh();
    });
    connect(bridge, &DebugBridge::consoleChanged, this, [this] {
        Pending = false;
        HavePrev = false;
        autoLoadSymbols();
        refresh();
    });
    connect(bridge, &DebugBridge::symbolsChanged, this, &DebuggerWindow::updateStatus);

    bridge->retainCallTracking();
    Tracking = true;
    autoLoadSymbols();

    Config::Table& cfg = Bridge->instance()->getGlobalConfig();
    QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(cfg.GetString("Debugger.Geometry")));
    QByteArray state = QByteArray::fromBase64(QByteArray::fromStdString(cfg.GetString("Debugger.Layout")));
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    if (!state.isEmpty())
        restoreState(state);
}

DebuggerWindow::~DebuggerWindow()
{
    OpenWindows.remove(Bridge);
}

void DebuggerWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (Interval > 0)
        Timer->start(Interval);
    FollowPC = true;
    refresh();
}

void DebuggerWindow::closeEvent(QCloseEvent* event)
{
    Timer->stop();
    Config::Table& cfg = Bridge->instance()->getGlobalConfig();
    cfg.SetString("Debugger.Geometry", saveGeometry().toBase64().toStdString());
    cfg.SetString("Debugger.Layout", saveState().toBase64().toStdString());
    Config::Save();
    if (Tracking)
        Bridge->releaseCallTracking();
    Tracking = false;
    QMainWindow::closeEvent(event);
}

int DebuggerWindow::cpu() const
{
    return CPUBox->currentIndex();
}

void DebuggerWindow::goTo(u32 addr)
{
    Disasm->goTo(addr);
}

void DebuggerWindow::buildToolbar()
{
    QToolBar* bar = addToolBar("Run");
    bar->setObjectName("RunToolbar");
    ActBreak = bar->addAction("Break", this, [this] { Bridge->requestBreak(cpu()); });
    ActBreak->setShortcut(Qt::Key_F6);
    ActContinue = bar->addAction("Continue", this, [this] { Bridge->resume(); });
    ActContinue->setShortcut(Qt::Key_F5);
    ActStep = bar->addAction("Step into", this, [this] { Bridge->step(cpu(), (u32)StepCount->value()); });
    ActStep->setShortcut(Qt::Key_F11);
    ActOver = bar->addAction("Step over", this, [this] { Bridge->stepOver(cpu()); });
    ActOver->setShortcut(Qt::Key_F10);
    ActOut = bar->addAction("Step out", this, [this] { Bridge->stepOut(cpu()); });
    ActOut->setShortcut(Qt::SHIFT | Qt::Key_F11);
    QAction* runTo = bar->addAction("Run to cursor", this, [this] { Bridge->runTo(cpu(), Disasm->selected()); });
    runTo->setShortcut(Qt::Key_F4);
    for (QAction* a : {ActBreak, ActContinue, ActStep, ActOver, ActOut, runTo})
        a->setToolTip(a->text() + " (" + a->shortcut().toString() + ")");
    bar->addWidget(new QLabel(" Steps ", this));
    StepCount = new QSpinBox(this);
    StepCount->setObjectName("StepCount");
    StepCount->setRange(1, MaxStepCount);
    StepCount->setToolTip("Step into runs this many instructions");
    bar->addWidget(StepCount);
    bar->addSeparator();

    CPUBox = new QComboBox(this);
    CPUBox->addItems({"ARM9", "ARM7"});
    bar->addWidget(CPUBox);
    connect(CPUBox, &QComboBox::currentIndexChanged, this, [this](int c) {
        Disasm->setCPU(c);
        FollowPC = true;
        applySnapshot(Snap);
    });

    bar->addWidget(new QLabel(" Go to: ", this));
    GotoEdit = new QLineEdit(this);
    GotoEdit->setPlaceholderText("address or symbol[+off]");
    GotoEdit->setMaximumWidth(260);
    bar->addWidget(GotoEdit);
    connect(GotoEdit, &QLineEdit::returnPressed, this, [this] {
        u32 addr;
        if (Bridge->symbols().parseAddress(SymStr(GotoEdit->text()), addr))
            Disasm->goTo(addr);
        else
            GotoEdit->selectAll();
    });
    QAction* gotoFocus = new QAction(this);
    gotoFocus->setShortcut(Qt::CTRL | Qt::Key_G);
    connect(gotoFocus, &QAction::triggered, this, [this] {
        GotoEdit->setFocus();
        GotoEdit->selectAll();
    });
    addAction(gotoFocus);
    bar->addAction("PC", this, [this] { Disasm->goToPC(); })->setToolTip("Show the current instruction");
    bar->addAction("Back", Disasm, &DisasmView::back)->setToolTip("Back (Backspace)");
    bar->addSeparator();
    bar->addAction("Symbols...", this, &DebuggerWindow::loadSymbols)
        ->setToolTip("Load a dsd config.yaml (or a folder holding one), or a text file of 'address name [size]' lines");
    bar->addAction("Labels...", this, &DebuggerWindow::loadLabels)
        ->setToolTip("Layer a text file of 'address name [module]' lines over the symbols; cancel the dialog to remove them");
    QAction* demangle = bar->addAction("Demangle");
    demangle->setCheckable(true);
    demangle->setChecked(Bridge->symbols().demangle());
    demangle->setToolTip("Show C++ names demangled; Go to accepts either form");
    connect(demangle, &QAction::toggled, this, [this](bool on) {
        Bridge->symbols().setDemangle(on);
        emit Bridge->symbolsChanged();
        applySnapshot(Snap);
    });
}

void DebuggerWindow::buildViewToolbar()
{
    QToolBar* bar = addToolBar("View");
    bar->setObjectName("ViewToolbar");

    DecodeButton = new QToolButton(this);
    DecodeButton->setPopupMode(QToolButton::InstantPopup);
    DecodeButton->setToolTip("Decode the listing as ARM or Thumb, or per symbol and CPSR");
    QMenu* menu = new QMenu(DecodeButton);
    QActionGroup* group = new QActionGroup(menu);
    const char* names[3] = {"Auto (symbols, CPSR)", "ARM", "Thumb"};
    for (int m = 0; m < 3; m++)
    {
        QAction* act = menu->addAction(names[m]);
        act->setCheckable(true);
        group->addAction(act);
        connect(act, &QAction::triggered, this, [this, m] { Disasm->setMode((DisasmView::Mode)m); });
        DecodeActions[m] = act;
    }
    DecodeButton->setMenu(menu);
    bar->addWidget(DecodeButton);
    connect(Disasm, &DisasmView::modeChanged, this, &DebuggerWindow::showDecodeMode);
    showDecodeMode(Disasm->mode());
    bar->addSeparator();

    bar->addWidget(new QLabel(" Update every ", this));
    UpdateEvery = new QSpinBox(this);
    UpdateEvery->setObjectName("UpdateInterval");
    UpdateEvery->setRange(0, MaxUpdateMs);
    UpdateEvery->setSingleStep(UpdateStepMs);
    UpdateEvery->setSuffix(" ms");
    UpdateEvery->setSpecialValueText("off");
    UpdateEvery->setKeyboardTracking(false);
    UpdateEvery->setValue(Interval);
    UpdateEvery->setToolTip("How often registers, breakpoints and the listing refresh while the game runs");
    bar->addWidget(UpdateEvery);
    connect(UpdateEvery, &QSpinBox::valueChanged, this, &DebuggerWindow::setUpdateInterval);

    QAction* follow = bar->addAction("Follow PC");
    follow->setCheckable(true);
    follow->setToolTip("While the game runs, keep the current instruction in view on every update");
    connect(follow, &QAction::toggled, this, [this](bool on) {
        FollowLive = on;
        refresh();
    });
    bar->addAction("Refresh", this, [this] {
        refresh();
        Disasm->reload();
    })->setToolTip("Read registers, breakpoints and the listing again now");
}

void DebuggerWindow::setUpdateInterval(int ms)
{
    Interval = ms;
    if (ms <= 0)
        Timer->stop();
    else if (isVisible())
        Timer->start(ms);
}

void DebuggerWindow::showDecodeMode(int mode)
{
    static const char* const labels[3] = {"Decode: auto", "Decode: ARM", "Decode: Thumb"};
    if (mode < 0 || mode > 2)
        return;
    DecodeActions[mode]->setChecked(true);
    DecodeButton->setText(labels[mode]);
}

void DebuggerWindow::buildDocks()
{
    for (int c = 0; c < 2; c++)
    {
        QTableWidget* t = new QTableWidget(22, 2, this);
        t->setFont(MonoFont());
        t->horizontalHeader()->hide();
        t->verticalHeader()->hide();
        t->horizontalHeader()->setStretchLastSection(true);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        t->verticalHeader()->setDefaultSectionSize(t->fontMetrics().height() + 2);
        const char* extra[6] = {"cpsr", "flags", "spsr", "ime", "ie", "if"};
        for (int r = 0; r < 22; r++)
        {
            auto* name = new QTableWidgetItem(r < 16 ? RegNames[r] : extra[r - 16]);
            t->setItem(r, 0, name);
            t->setItem(r, 1, new QTableWidgetItem());
        }
        t->resizeColumnToContents(0);
        connect(t, &QTableWidget::cellDoubleClicked, this, [this, c](int row, int) { editRegister(c, row); });
        Regs[c] = t;
        QDockWidget* dock = new QDockWidget(c ? "ARM7 registers" : "ARM9 registers", this);
        dock->setObjectName(c ? "ARM7Regs" : "ARM9Regs");
        dock->setWidget(t);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        if (c)
            tabifyDockWidget(findChild<QDockWidget*>("ARM9Regs"), dock);
    }
    findChild<QDockWidget*>("ARM9Regs")->raise();

    QWidget* pw = new QWidget(this);
    QVBoxLayout* pl = new QVBoxLayout(pw);
    pl->setContentsMargins(0, 0, 0, 0);
    Points = new QTreeWidget(pw);
    Points->setHeaderLabels({"On", "ID", "Kind", "CPU", "Address", "Size", "Symbol", "Hits", "Owner"});
    Points->setRootIsDecorated(false);
    Points->setFont(MonoFont());
    pl->addWidget(Points);
    QHBoxLayout* pb = new QHBoxLayout();
    QPushButton* addBp = new QPushButton("Breakpoint...", pw);
    QPushButton* addWp = new QPushButton("Watchpoint...", pw);
    QPushButton* remove = new QPushButton("Remove", pw);
    QPushButton* clear = new QPushButton("Remove all", pw);
    for (QPushButton* b : {addBp, addWp, remove, clear})
        pb->addWidget(b);
    pb->addStretch();
    pl->addLayout(pb);
    connect(addBp, &QPushButton::clicked, this, [this] { addPointDialog(true); });
    connect(addWp, &QPushButton::clicked, this, [this] { addPointDialog(false); });
    connect(remove, &QPushButton::clicked, this, [this] {
        std::vector<int> ids;
        for (QTreeWidgetItem* item : Points->selectedItems())
            ids.push_back(item->data(1, Qt::DisplayRole).toInt());
        Bridge->post([ids](NDS& nds) {
            for (int id : ids)
                nds.Debugger.Remove(id);
        });
        refresh();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        Bridge->post([](NDS& nds) {
            std::vector<int> ids;
            for (auto& b : nds.Debugger.GetBreakpoints())
                if (!b.Callback)
                    ids.push_back(b.ID);
            for (auto& w : nds.Debugger.GetWatchpoints())
                if (!w.Callback)
                    ids.push_back(w.ID);
            for (int id : ids)
                nds.Debugger.Remove(id);
        });
        refresh();
    });
    connect(Points, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        if (col != 0)
            return;
        int id = item->data(1, Qt::DisplayRole).toInt();
        bool on = item->checkState(0) == Qt::Checked;
        Bridge->post([id, on](NDS& nds) { nds.Debugger.SetEnabled(id, on); });
        refresh();
    });
    connect(Points, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        if (col == 0)
            return;
        Disasm->goTo(item->data(4, Qt::UserRole).toUInt());
    });
    QDockWidget* pdock = new QDockWidget("Breakpoints / watchpoints", this);
    pdock->setObjectName("Points");
    pdock->setWidget(pw);
    addDockWidget(Qt::BottomDockWidgetArea, pdock);

    Stack = new QTreeWidget(this);
    Stack->setHeaderLabels({"#", "Function", "Called from", "Returns to", "SP", "Mode"});
    Stack->setRootIsDecorated(false);
    Stack->setFont(MonoFont());
    connect(Stack, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        Disasm->goTo(item->data(col == 1 ? 1 : 2, Qt::UserRole).toUInt());
    });
    QDockWidget* sdock = new QDockWidget("Call stack", this);
    sdock->setObjectName("CallStack");
    sdock->setWidget(Stack);
    addDockWidget(Qt::BottomDockWidgetArea, sdock);
    tabifyDockWidget(pdock, sdock);
    pdock->raise();
}

void DebuggerWindow::refresh()
{
    if (!Bridge->hasConsole())
    {
        Snap = {};
        updateActions();
        updateStatus();
        return;
    }
    if (Pending)
        return;
    Pending = true;
    auto set = Bridge->symbols().set();
    DebugBridge* bridge = Bridge;
    Bridge->call(this, [set, bridge](NDS& nds) {
        DebugSnapshot s {};
        for (int c = 0; c < 2; c++)
        {
            ARM& arm = c ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
            CPUSnapshot& o = s.CPU[c];
            memcpy(o.R, arm.R, sizeof(o.R));
            o.PC = DebugBridge::currentPC(nds, c);
            o.R[15] = o.PC;
            o.CPSR = arm.CPSR;
            o.SPSR = SPSRFor(arm);
            o.IME = nds.IME[c];
            o.IE = nds.IE[c];
            o.IF = nds.IF[c];
            o.Halted = arm.Halted;
            s.Stack[c] = nds.Debugger.GetCallStack(c);
        }
        for (auto& b : nds.Debugger.GetBreakpoints())
            s.Points.push_back({b.ID, true, b.CPUMask, 0, b.Enabled, b.Addr, b.Addr, b.Hits, (bool)b.Callback, bridge->isTemp(b.ID)});
        for (auto& w : nds.Debugger.GetWatchpoints())
            s.Points.push_back({w.ID, false, w.CPUMask, w.Flags, w.Enabled, w.Start, w.End, w.Hits, (bool)w.Callback, false});
        if (set)
            s.Overlays = DebugSymbols::detect(*set, nds);
        s.Frame = nds.NumFrames;
        s.Debug = nds.IsDebugMode();
        return s;
    }, [this](const DebugSnapshot& s) {
        Pending = false;
        applySnapshot(s);
    });
}

void DebuggerWindow::applySnapshot(const DebugSnapshot& s)
{
    bool stopped = Bridge->isBroken() || !Bridge->isRunning();
    if (stopped && !HavePrev)
    {
        Prev = s.CPU;
        HavePrev = true;
    }
    Snap = s;
    Bridge->symbols().setLoaded(s.Overlays);
    int c = cpu();
    bool live = FollowLive && !stopped;
    Disasm->setPC(s.CPU[c].PC, s.CPU[c].CPSR & 0x20, stopped || live);
    QSet<u32> exec, disabled;
    for (const PointInfo& p : s.Points)
        if (p.Exec && (p.CPUMask & (1 << c)))
        {
            exec.insert(p.Start);
            if (!p.Enabled)
                disabled.insert(p.Start);
        }
    Disasm->setBreakpoints(exec, disabled);
    if (FollowPC && stopped)
    {
        Disasm->goToPC();
        FollowPC = false;
    }
    else if (live)
        Disasm->goToPC();
    updateRegs(0);
    updateRegs(1);
    updatePoints();
    updateStack();
    updateActions();
    updateStatus();
    if (stopped)
        Prev = s.CPU;
}

void DebuggerWindow::updateRegs(int c)
{
    QTableWidget* t = Regs[c];
    const CPUSnapshot& s = Snap.CPU[c];
    const CPUSnapshot& p = Prev[c];
    QColor changed(220, 50, 50);
    QColor normal = t->palette().text().color();
    auto set = [&](int row, const QString& text, bool diff) {
        QTableWidgetItem* item = t->item(row, 1);
        item->setText(text);
        item->setForeground(diff ? changed : normal);
    };
    DebugSymbols& syms = Bridge->symbols();
    for (int r = 0; r < 16; r++)
    {
        QString text = Hex(s.R[r]);
        QString sym = (r >= 13 || (s.R[r] >> 24) == 0x02 || (s.R[r] >> 24) == 0x01) ? SymStr(syms.describe(s.R[r])) : QString();
        if (!sym.isEmpty())
            text += "  " + sym;
        set(r, text, HavePrev && s.R[r] != p.R[r]);
    }
    set(16, Hex(s.CPSR), HavePrev && s.CPSR != p.CPSR);
    set(17, Flags(s.CPSR) + (s.Halted ? "  halted" : ""), false);
    set(18, (s.CPSR & 0x1F) == 0x10 || (s.CPSR & 0x1F) == 0x1F ? QString("-") : Hex(s.SPSR), false);
    set(19, QString::number(s.IME & 1), HavePrev && s.IME != p.IME);
    set(20, Hex(s.IE), HavePrev && s.IE != p.IE);
    set(21, Hex(s.IF), HavePrev && s.IF != p.IF);
}

void DebuggerWindow::updatePoints()
{
    QSignalBlocker block(Points);
    QSet<int> selected;
    for (QTreeWidgetItem* item : Points->selectedItems())
        selected.insert(item->data(1, Qt::DisplayRole).toInt());
    Points->clear();
    DebugSymbols& syms = Bridge->symbols();
    for (const PointInfo& p : Snap.Points)
    {
        auto* item = new QTreeWidgetItem(Points);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, p.Enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(1, Qt::DisplayRole, p.ID);
        QString kind = p.Exec ? "exec" : QString(p.Flags & Watch_Read ? "r" : "") + (p.Flags & Watch_Write ? "w" : "");
        item->setText(2, kind);
        item->setText(3, p.CPUMask == 3 ? "both" : p.CPUMask == 1 ? "ARM9" : "ARM7");
        item->setText(4, Hex(p.Start));
        item->setData(4, Qt::UserRole, p.Start);
        item->setText(5, p.Exec ? QString() : "0x" + QString::number(p.End - p.Start + 1, 16));
        item->setText(6, SymStr(syms.describe(p.Start)));
        item->setText(7, QString::number(p.Hits));
        item->setText(8, p.Temp ? "temp" : p.Scripted ? "script" : "user");
        if (selected.contains(p.ID))
            item->setSelected(true);
    }
    for (int i = 0; i < Points->columnCount(); i++)
        Points->resizeColumnToContents(i);
}

void DebuggerWindow::updateStack()
{
    Stack->clear();
    int c = cpu();
    const auto& frames = Snap.Stack[c];
    DebugSymbols& syms = Bridge->symbols();
    for (int i = (int)frames.size() - 1, n = 0; i >= 0; i--, n++)
    {
        const CallFrame& f = frames[i];
        auto* item = new QTreeWidgetItem(Stack);
        item->setText(0, QString::number(n));
        QString fn = SymStr(syms.describe(f.Entry));
        item->setText(1, (f.Exception ? "[exception] " : "") + (fn.isEmpty() ? Hex(f.Entry) : fn));
        item->setData(1, Qt::UserRole, f.Entry);
        QString site = SymStr(syms.describe(f.Site));
        item->setText(2, Hex(f.Site) + (site.isEmpty() ? "" : "  " + site));
        item->setData(2, Qt::UserRole, f.Site);
        item->setText(3, Hex(f.Return));
        item->setText(4, Hex(f.SP));
        item->setText(5, ModeName(f.Mode));
    }
    if (frames.empty() && !Bridge->callTracking())
        new QTreeWidgetItem(Stack, QStringList{"", "call tracking is off"});
    for (int i = 0; i < Stack->columnCount(); i++)
        Stack->resizeColumnToContents(i);
}

void DebuggerWindow::updateActions()
{
    bool console = Bridge->hasConsole();
    bool broken = Bridge->isBroken();
    bool running = Bridge->isRunning();
    bool debug = console && Snap.Debug;
    ActBreak->setEnabled(debug && !broken);
    ActContinue->setEnabled(console && (broken || !running));
    for (QAction* a : {ActStep, ActOver, ActOut})
        a->setEnabled(debug && (broken || !running));
}

QString DebuggerWindow::describeBreak(const BreakEvent& ev) const
{
    DebugSymbols& syms = Bridge->symbols();
    QString cpuName = ev.CPU ? "ARM7" : "ARM9";
    QString where = Hex(ev.PC);
    QString sym = SymStr(syms.describe(ev.PC));
    if (!sym.isEmpty())
        where += " " + sym;
    switch (ev.Kind)
    {
    case BreakKind::Exec:
        return QString("%1 breakpoint #%2 at %3").arg(cpuName).arg(ev.ID).arg(where);
    case BreakKind::Read:
    case BreakKind::Write:
    {
        QString what = QString("%1 %2 0x%3 (%4 bytes) = 0x%5")
            .arg(ev.Kind == BreakKind::Write ? "write" : "read")
            .arg(ev.Kind == BreakKind::Write ? "to" : "from")
            .arg(Hex(ev.Addr)).arg(ev.Size).arg(Hex(ev.Value, ev.Size * 2));
        QString target = SymStr(syms.describe(ev.Addr));
        if (!target.isEmpty())
            what += " [" + target + "]";
        if (ev.DMAChannel != 0xFF)
            return QString("%1 DMA%2 %3 (watchpoint #%4)").arg(cpuName).arg(ev.DMAChannel).arg(what).arg(ev.ID);
        return QString("%1 watchpoint #%2: %3 by %4").arg(cpuName).arg(ev.ID).arg(what).arg(where);
    }
    case BreakKind::Step:
        return QString("%1 step at %2").arg(cpuName).arg(where);
    case BreakKind::Request:
        return QString("%1 break at %2").arg(cpuName).arg(where);
    case BreakKind::Fault:
        return QString("%1 fault at %2, address 0x%3").arg(cpuName).arg(where).arg(Hex(ev.Addr));
    }
    return QString();
}

void DebuggerWindow::updateStatus()
{
    QString text;
    if (!Bridge->hasConsole())
        text = "No console running";
    else if (!Snap.Debug)
        text = "JIT is on: breakpoints, stepping and tracing need the interpreter (Config > Emu settings > disable JIT)";
    else if (Bridge->isBroken())
        text = "Stopped: " + describeBreak(Bridge->lastBreak());
    else if (Bridge->isRunning())
        text = QString("Running, frame %1").arg(Snap.Frame);
    else
        text = QString("Paused, frame %1").arg(Snap.Frame);
    Status->setText(text);

    DebugSymbols& syms = Bridge->symbols();
    if (syms.empty())
        SymbolStatus->setText("no symbols");
    else
    {
        QStringList ovs;
        if (syms.set())
            for (int id : syms.loaded())
                for (const auto& mod : syms.set()->Modules)
                    if (mod.Overlay == id)
                        ovs << SymStr(mod.Name);
        SymbolStatus->setText(QString("%1 symbols, overlays: %2").arg(syms.set()->SymbolCount).arg(ovs.isEmpty() ? "none" : ovs.join(' ')));
        SymbolStatus->setToolTip(SymStr(syms.source()));
    }
}

void DebuggerWindow::toggleBreakpoint(u32 addr)
{
    int c = cpu();
    Bridge->post([addr, c](NDS& nds) {
        for (auto& b : nds.Debugger.GetBreakpoints())
            if (b.Addr == (addr & ~1u) && (b.CPUMask & (1 << c)) && !b.Callback)
            {
                nds.Debugger.Remove(b.ID);
                return;
            }
        nds.Debugger.AddBreakpoint(1 << c, addr);
    });
    refresh();
}

void DebuggerWindow::addPointDialog(bool exec)
{
    QDialog dlg(this);
    dlg.setWindowTitle(exec ? "Add breakpoint" : "Add watchpoint");
    QFormLayout* form = new QFormLayout(&dlg);
    QLineEdit* addr = new QLineEdit(&dlg);
    addr->setPlaceholderText("02001234, symbol, or symbol+0x10");
    if (exec)
        addr->setText(Hex(Disasm->selected()));
    QComboBox* cpuBox = new QComboBox(&dlg);
    cpuBox->addItems({"ARM9", "ARM7", "both"});
    cpuBox->setCurrentIndex(cpu());
    form->addRow("Address", addr);
    form->addRow("CPU", cpuBox);
    QLineEdit* size = nullptr;
    QComboBox* kind = nullptr;
    if (!exec)
    {
        size = new QLineEdit("4", &dlg);
        kind = new QComboBox(&dlg);
        kind->addItems({"write", "read", "read or write"});
        form->addRow("Size (bytes)", size);
        form->addRow("Break on", kind);
        form->addRow(new QLabel("Writes by DMA also hit write watchpoints.", &dlg));
    }
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;

    u32 start;
    if (!Bridge->symbols().parseAddress(SymStr(addr->text()), start))
    {
        QMessageBox::warning(this, "melonDS", "Cannot read the address: " + addr->text());
        return;
    }
    u8 mask = cpuBox->currentIndex() == 2 ? 3 : 1 << cpuBox->currentIndex();
    if (exec)
    {
        Bridge->post([mask, start](NDS& nds) { nds.Debugger.AddBreakpoint(mask, start); });
    }
    else
    {
        bool ok;
        QString st = size->text().trimmed();
        u32 len = st.startsWith("0x", Qt::CaseInsensitive) ? st.mid(2).toUInt(&ok, 16) : st.toUInt(&ok, 0);
        if (!ok || !len)
            len = 4;
        u8 flags = kind->currentIndex() == 0 ? Watch_Write : kind->currentIndex() == 1 ? Watch_Read : (Watch_Read | Watch_Write);
        Bridge->post([mask, start, len, flags](NDS& nds) { nds.Debugger.AddWatchpoint(mask, start, len, flags); });
    }
    refresh();
}

void DebuggerWindow::editRegister(int c, int row)
{
    if (row > 16 || !(Bridge->isBroken() || !Bridge->isRunning()))
        return;
    const CPUSnapshot& s = Snap.CPU[c];
    u32 cur = row < 16 ? s.R[row] : s.CPSR;
    bool ok;
    QString text = QInputDialog::getText(this, "Set register",
        QString("%1 %2 (hex)").arg(c ? "ARM7" : "ARM9").arg(row < 16 ? RegNames[row] : "cpsr flags"),
        QLineEdit::Normal, Hex(cur), &ok);
    if (!ok)
        return;
    u32 value = text.trimmed().remove("0x", Qt::CaseInsensitive).toUInt(&ok, 16);
    if (!ok)
        return;
    Bridge->post([c, row, value](NDS& nds) {
        ARM& arm = c ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
        if (row == 15)
            arm.JumpTo(value | ((arm.CPSR & 0x20) ? 1 : 0));
        else if (row == 16)
            arm.CPSR = (arm.CPSR & 0x07FFFFFF) | (value & 0xF8000000);
        else
            arm.R[row] = value;
    });
    refresh();
}

void DebuggerWindow::setPC(u32 addr)
{
    int c = cpu();
    Bridge->post([c, addr](NDS& nds) {
        ARM& arm = c ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
        arm.JumpTo(addr | ((arm.CPSR & 0x20) ? 1 : 0));
    });
    FollowPC = true;
    refresh();
}

void DebuggerWindow::loadSymbols()
{
    QString path = QFileDialog::getOpenFileName(this, "Load symbols", QString(),
        "dsd config or symbol list (*.yaml *.yml *.txt *.sym);;All files (*)");
    if (path.isEmpty())
        return;
    std::string error;
    if (!Bridge->symbols().load(SymStr(path), error))
    {
        QMessageBox::warning(this, "melonDS", SymStr(error));
        return;
    }
    DebugTools::rememberSymbols(Bridge, path);
    emit Bridge->symbolsChanged();
    refresh();
    Disasm->reload();
}

void DebuggerWindow::loadLabels()
{
    DebugSymbols& symbols = Bridge->symbols();
    QString current = SymStr(symbols.labelsSource());
    QString path = QFileDialog::getOpenFileName(this, "Load labels", current,
        "Labels (*.txt *.labels);;All files (*)");
    if (path.isEmpty())
    {
        if (current.isEmpty()
            || QMessageBox::question(this, "melonDS", "Remove the labels from " + current + "?")
                   != QMessageBox::Yes)
            return;
        symbols.clearLabels();
    }
    else
    {
        std::string error;
        if (!symbols.loadLabels(SymStr(path), error))
        {
            QMessageBox::warning(this, "melonDS", SymStr(error));
            return;
        }
        if (symbols.labelsUnmatched())
            QMessageBox::information(this, "melonDS",
                QString("%1 of %2 labels match no module in the loaded symbols and were skipped.")
                    .arg(symbols.labelsUnmatched()).arg(symbols.labelCount()));
    }
    DebugTools::rememberLabels(Bridge, path);
    emit Bridge->symbolsChanged();
    refresh();
    Disasm->reload();
}

void DebuggerWindow::autoLoadSymbols()
{
    DebugTools::autoLoadSymbols(Bridge);
}
