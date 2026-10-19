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

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>

#include "Automation.h"
#include "DebugBridge.h"
#include "DebugSymbols.h"
#include "DebugTools.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "Window.h"
#include "main.h"
#include "lua/LuaMain.h"
#include "DebugTool.h"
#include "ARM.h"

using namespace melonDS;

void Automation::start(EmuInstance* inst)
{
    QString dir = qEnvironmentVariable("MELONDS_AUTOMATION");
    if (dir.isEmpty())
        return;
    if (inst->getInstanceID())
        dir += "/" + QString::number(inst->getInstanceID());
    new Automation(inst, dir);
}

Automation::Automation(EmuInstance* inst, const QString& dir) : QObject(qApp), Inst(inst), Dir(dir)
{
    QDir().mkpath(dir);
    Timer = new QTimer(this);
    Timer->setInterval(50);
    connect(Timer, &QTimer::timeout, this, &Automation::poll);
    Timer->start();
}

static QWidget* FindWindow(QString title)
{
    title.replace('_', ' ');
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->windowTitle().contains(title, Qt::CaseInsensitive))
            return w;
    return nullptr;
}

static u32 Num(const QString& s)
{
    QString t = s.startsWith("0x", Qt::CaseInsensitive) ? s.mid(2) : s;
    return t.toUInt(nullptr, 16);
}

void Automation::poll()
{
    QFile cmd(QDir(Dir).filePath("cmd"));
    if (!cmd.exists())
        return;
    if (!cmd.open(QIODevice::ReadOnly))
        return;
    QStringList lines = QString::fromUtf8(cmd.readAll()).split('\n', Qt::SkipEmptyParts);
    cmd.close();
    cmd.remove();
    QString out;
    for (const QString& line : lines)
    {
        QStringList args = line.trimmed().split(' ', Qt::SkipEmptyParts);
        if (args.isEmpty())
            continue;
        out += run(args) + "\n";
    }
    QFile tmp(QDir(Dir).filePath("out.tmp"));
    if (tmp.open(QIODevice::WriteOnly))
    {
        tmp.write(out.toUtf8());
        tmp.close();
        QFile::remove(QDir(Dir).filePath("out"));
        tmp.rename(QDir(Dir).filePath("out"));
    }
}

QString Automation::run(const QStringList& args)
{
    const QString& c = args[0];
    DebugBridge* bridge = Inst->getDebugBridge();
    EmuThread* thread = Inst->getEmuThread();
    MainWindow* win = Inst->getMainWindow();
    QString rest = args.mid(1).join(' ');

    if (c == "state")
    {
        BreakEvent ev = bridge->lastBreak();
        return QString("console=%1 debug=%2 broken=%3 running=%4 lastpc=%5 kind=%6")
            .arg(bridge->hasConsole()).arg(bridge->debugMode()).arg(bridge->isBroken()).arg(bridge->isRunning())
            .arg(Hex(ev.PC)).arg((int)ev.Kind);
    }
    if (c == "windows")
    {
        QStringList titles;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->isVisible())
                titles << w->windowTitle();
        return titles.join(" | ");
    }
    if (c == "open" && args.size() > 1)
    {
        QWidget* w = DebugTools::openByName(args[1], bridge, win);
        if (!w && args[1] == "memview")
            QMetaObject::invokeMethod(win, "onMemView");
        if (!w && args[1] == "lua")
            QMetaObject::invokeMethod(win, "onOpenLuaScript");
        return "ok";
    }
    if (c == "lua" && args.size() > 1)
    {
        QMetaObject::invokeMethod(win, "onOpenLuaScript");
        LuaConsoleDialog* dlg = win->getLuaDialog();
        if (!dlg)
            return "error: no lua dialog";
        dlg->currentScript = QFileInfo(rest);
        dlg->getLuaBundle()->flagNewLua = true;
        return "ok";
    }
    if (c == "grab" && args.size() > 2)
    {
        QWidget* w = args[1] == "main" ? (QWidget*)win : FindWindow(args[1]);
        if (!w)
            return "error: no window " + args[1];
        return w->grab().save(args[2]) ? "ok" : "error: save";
    }
    if (c == "resize" && args.size() > 3)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        w->resize(args[2].toInt(), args[3].toInt());
        return "ok";
    }
    if (c == "action" && args.size() > 2)
    {
        QWidget* w = args[1] == "main" ? (QWidget*)win : FindWindow(args[1]);
        if (!w)
            return "error: no window";
        QString text = args.mid(2).join(' ');
        for (QAction* a : w->findChildren<QAction*>())
            if (a->text().remove('&') == text)
            {
                a->trigger();
                return "ok";
            }
        for (QAbstractButton* b : w->findChildren<QAbstractButton*>())
            if (b->text().remove('&') == text)
            {
                b->click();
                return "ok";
            }
        return "error: no action " + text;
    }
    if (c == "edit" && args.size() > 3)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        for (QLineEdit* e : w->findChildren<QLineEdit*>())
            if (e->objectName() == args[2] || e->placeholderText().startsWith(args[2]))
            {
                e->setText(args.mid(3).join(' '));
                emit e->returnPressed();
                emit e->editingFinished();
                return "ok";
            }
        return "error: no edit";
    }
    if (c == "select" && args.size() > 3)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        auto views = w->findChildren<QAbstractItemView*>();
        int n = args[2].toInt();
        if (n >= views.size())
            return "error: no view";
        QAbstractItemView* v = views[n];
        QModelIndex idx = v->model()->index(args[3].toInt(), 0);
        v->setCurrentIndex(idx);
        emit v->clicked(idx);
        return "ok";
    }
    if (c == "tab" && args.size() > 2)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        QString text = args.mid(2).join(' ');
        for (QTabWidget* t : w->findChildren<QTabWidget*>())
            for (int i = 0; i < t->count(); i++)
                if (t->tabText(i).remove('&') == text)
                {
                    t->setCurrentIndex(i);
                    return "ok";
                }
        for (QTabBar* t : w->findChildren<QTabBar*>())
            for (int i = 0; i < t->count(); i++)
                if (t->tabText(i).remove('&') == text)
                {
                    t->setCurrentIndex(i);
                    return "ok";
                }
        return "error: no tab";
    }
    if (c == "combo" && args.size() > 3)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        int n = args[2].toInt();
        auto boxes = w->findChildren<QComboBox*>();
        if (n >= boxes.size())
            return "error: no combo";
        int idx = boxes[n]->findText(args.mid(3).join(' '));
        if (idx < 0)
            idx = args[3].toInt();
        boxes[n]->setCurrentIndex(idx);
        return "ok";
    }
    if (c == "key" && args.size() > 2)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        QKeySequence seq(args[2]);
        if (seq.isEmpty())
            return "error: key";
        QKeyCombination kc = seq[0];
        QWidget* target = w->focusWidget() ? w->focusWidget() : w;
        QApplication::sendEvent(target, new QKeyEvent(QEvent::ShortcutOverride, kc.key(), kc.keyboardModifiers()));
        QKeyEvent press(QEvent::KeyPress, kc.key(), kc.keyboardModifiers());
        QApplication::sendEvent(target, &press);
        return "ok";
    }
    if (c == "hold" && args.size() > 2)
    {
        QKeySequence seq(args[1]);
        if (seq.isEmpty())
            return "error: key";
        int key = seq[0].key();
        int ms = args[2].toInt();
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(win, &press);
        QTimer::singleShot(ms, win, [win, key] {
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QApplication::sendEvent(win, &release);
        });
        return "ok";
    }
    if (c == "text" && args.size() > 1)
    {
        QWidget* w = FindWindow(args[1]);
        if (!w)
            return "error: no window";
        QStringList parts;
        for (QLabel* l : w->findChildren<QLabel*>())
            if (l->isVisible() && !l->text().isEmpty())
                parts << "label: " + l->text();
        for (QTreeWidget* t : w->findChildren<QTreeWidget*>())
            for (int i = 0; i < std::min(t->topLevelItemCount(), 40); i++)
            {
                QStringList cols;
                for (int k = 0; k < t->columnCount(); k++)
                    cols << t->topLevelItem(i)->text(k);
                parts << "tree " + t->objectName() + ": " + cols.join(" | ");
            }
        for (QTableWidget* t : w->findChildren<QTableWidget*>())
            for (int r = 0; r < std::min(t->rowCount(), 40); r++)
            {
                QStringList cols;
                for (int k = 0; k < t->columnCount(); k++)
                    cols << (t->item(r, k) ? t->item(r, k)->text() : QString());
                parts << "table " + t->objectName() + ": " + cols.join(" | ");
            }
        for (QPlainTextEdit* e : w->findChildren<QPlainTextEdit*>())
            parts << "textedit: " + e->toPlainText().right(2000);
        return parts.join("\n");
    }
    if (c == "symbols")
    {
        QString error;
        if (!bridge->symbols().load(rest, error))
            return "error: " + error;
        emit bridge->symbolsChanged();
        return QString("ok %1").arg(bridge->symbols().set()->SymbolCount);
    }
    if (c == "newinstance")
        return createEmuInstance() ? "ok" : "error";
    if (c == "boot")
    {
        QString error;
        return thread->bootROM({rest}, error) ? "ok" : "error: " + error;
    }
    if (c == "ascii" && args.size() > 2)
    {
        u32 addr = Num(args[1]);
        u32 len = Num(args[2]);
        QByteArray bytes(len, 0);
        bool ok = bridge->run([&](NDS& nds) {
            for (u32 i = 0; i < len; i++)
                bytes[i] = (char)nds.Debugger.Peek(0, addr + i, 1);
        });
        if (!ok)
            return "error: timeout";
        QStringList runs;
        QString cur;
        for (char ch : bytes)
        {
            if (ch >= 0x20 && ch < 0x7F)
                cur += ch;
            else
            {
                if (cur.size() >= 4)
                    runs << cur;
                cur.clear();
            }
        }
        if (cur.size() >= 4)
            runs << cur;
        return runs.join(" | ");
    }
    if (c == "loadstate")
        return thread->loadState(rest) ? "ok" : "error";
    if (c == "savestate")
        return thread->saveState(rest) ? "ok" : "error";
    if (c == "pause")
    {
        thread->emuPause();
        return "ok";
    }
    if (c == "unpause")
    {
        thread->emuUnpause();
        return "ok";
    }
    if (c == "break")
    {
        bridge->requestBreak(args.size() > 1 ? args[1].toInt() : 0);
        return "ok";
    }
    if (c == "continue")
    {
        bridge->resume();
        return "ok";
    }
    if (c == "step")
    {
        bridge->step(args.size() > 1 ? args[1].toInt() : 0);
        return "ok";
    }
    if (c == "stepover")
    {
        bridge->stepOver(args.size() > 1 ? args[1].toInt() : 0);
        return "ok";
    }
    if (c == "stepout")
    {
        bridge->stepOut(args.size() > 1 ? args[1].toInt() : 0);
        return "ok";
    }
    if (c == "runto" && args.size() > 1)
    {
        u32 addr;
        if (!bridge->symbols().parseAddress(args[1], addr))
            return "error: address";
        bridge->runTo(0, addr);
        return "ok";
    }
    if (c == "bp" && args.size() > 1)
    {
        u32 addr;
        if (!bridge->symbols().parseAddress(args[1], addr))
            return "error: address";
        int id = -1;
        bridge->run([&](NDS& nds) { id = nds.Debugger.AddBreakpoint(1, addr); });
        return QString("ok %1 %2").arg(id).arg(Hex(addr));
    }
    if (c == "wp" && args.size() > 3)
    {
        u32 addr;
        if (!bridge->symbols().parseAddress(args[1], addr))
            return "error: address";
        u32 size = args[2].toUInt();
        u8 flags = (args[3].contains('r') ? Watch_Read : 0) | (args[3].contains('w') ? Watch_Write : 0);
        int id = -1;
        bridge->run([&](NDS& nds) { id = nds.Debugger.AddWatchpoint(1, addr, size, flags); });
        return QString("ok %1").arg(id);
    }
    if (c == "clearpoints")
    {
        bridge->run([](NDS& nds) { nds.Debugger.ClearAll(); });
        return "ok";
    }
    if (c == "peek" && args.size() > 1)
    {
        u32 addr = Num(args[1]);
        u32 v = 0;
        bool ok = bridge->run([&](NDS& nds) { v = nds.Debugger.Peek(0, addr, 4); });
        return ok ? Hex(v) : "error: timeout";
    }
    if (c == "regs")
    {
        QString s;
        bool ok = bridge->run([&](NDS& nds) {
            for (int cpu = 0; cpu < 2; cpu++)
            {
                ARM& arm = cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
                s += QString("arm%1 pc=%2 lr=%3 sp=%4 cpsr=%5 r0=%6\n").arg(cpu ? 7 : 9)
                    .arg(Hex(DebugBridge::currentPC(nds, cpu))).arg(Hex(arm.R[14])).arg(Hex(arm.R[13]))
                    .arg(Hex(arm.CPSR)).arg(Hex(arm.R[0]));
            }
        });
        return ok ? s.trimmed() : "error: timeout";
    }
    if (c == "quit")
    {
        QTimer::singleShot(100, qApp, &QApplication::quit);
        return "ok";
    }
    return "error: unknown " + c;
}
