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

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include "DisasmView.h"
#include "DebugSymbolsQt.h"
#include "DebugTool.h"

using namespace melonDS;

constexpr u32 FetchBefore = 0x400;
constexpr u32 FetchAfter = 0x400;

DisasmView::DisasmView(DebugBridge* bridge, QWidget* parent) : QAbstractScrollArea(parent), Bridge(bridge)
{
    setFont(MonoFont());
    setFocusPolicy(Qt::StrongFocus);
    verticalScrollBar()->setRange(0, 0x3FFFFFFF);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        u32 top = (u32)value << 2;
        if ((top & ~3u) != (Top & ~3u))
        {
            Top = top;
            fetch();
        }
    });
    connect(bridge, &DebugBridge::symbolsChanged, this, [this] {
        layoutRows();
        viewport()->update();
    });
    syncScroll();
}

void DisasmView::setCPU(int cpu)
{
    CPU = cpu;
    fetch();
}

void DisasmView::setMode(Mode mode)
{
    ViewMode = mode;
    Thumb = mode == mode_Thumb || (mode == mode_Auto && PCValid && PCThumb);
    layoutRows();
    viewport()->update();
    emit modeChanged(mode);
}

void DisasmView::setPC(u32 pc, bool thumb, bool valid)
{
    PC = pc;
    PCThumb = thumb;
    PCValid = valid;
    viewport()->update();
}

void DisasmView::setBreakpoints(const QSet<u32>& exec, const QSet<u32>& disabled)
{
    Breaks = exec;
    Disabled = disabled;
    viewport()->update();
}

void DisasmView::goTo(u32 addr, bool record)
{
    if (record && (History.empty() || History.back() != Top))
        History.push_back(Top);
    if (History.size() > 256)
        History.erase(History.begin());
    select(addr & ~1u);
    if (ViewMode == mode_Auto)
    {
        const DebugSymbols::Symbol* sym = Bridge->symbols().lookup(addr);
        if (sym && sym->Kind == "function")
            Thumb = sym->Thumb;
        else if (addr & 1)
            Thumb = true;
    }
    int above = std::max(2, visibleLines() / 4);
    Top = (addr & ~1u) - above * step();
    syncScroll();
    fetch();
}

void DisasmView::goToPC()
{
    if (!PCValid)
        return;
    if (ViewMode == mode_Auto)
        Thumb = PCThumb;
    u32 lines = visibleLines();
    u32 span = lines * step();
    if (PC >= Top + step() * 2 && PC < Top + span - step() * 3)
    {
        select(PC);
        fetch();
        return;
    }
    goTo(PC | (PCThumb ? 1 : 0), false);
}

void DisasmView::back()
{
    if (History.empty())
        return;
    Top = History.back();
    History.pop_back();
    syncScroll();
    fetch();
}

void DisasmView::reload()
{
    fetch();
}

int DisasmView::lineHeight() const
{
    return fontMetrics().height();
}

int DisasmView::visibleLines() const
{
    return std::max(1, viewport()->height() / lineHeight());
}

void DisasmView::syncScroll()
{
    QSignalBlocker block(verticalScrollBar());
    verticalScrollBar()->setValue((int)(Top >> 2));
    verticalScrollBar()->setPageStep(visibleLines());
}

void DisasmView::fetch()
{
    int id = ++Request;
    u32 start = Top - FetchBefore;
    u32 len = FetchBefore + visibleLines() * 4 + FetchAfter;
    int cpu = CPU;
    auto set = Bridge->symbols().set();
    Bridge->call(this, [cpu, start, len, set](NDS& nds) {
        QByteArray out(len, 0);
        for (u32 i = 0; i < len; i += 4)
        {
            u32 v = nds.Debugger.Peek(cpu, start + i, 4);
            memcpy(out.data() + i, &v, 4);
        }
        std::vector<int> overlays;
        if (set)
            overlays = DebugSymbols::detect(*set, nds);
        return std::make_pair(out, overlays);
    }, [this, id, start](const std::pair<QByteArray, std::vector<int>>& result) {
        if (id != Request)
            return;
        BufBase = start;
        Buf = result.first;
        Bridge->symbols().setLoaded(result.second);
        layoutRows();
        viewport()->update();
    });
    layoutRows();
    viewport()->update();
}

u32 DisasmView::readBuf(u32 addr, int size, bool& ok) const
{
    u32 off = addr - BufBase;
    ok = off < (u32)Buf.size() && off + size <= (u32)Buf.size();
    if (!ok)
        return 0;
    u32 v = 0;
    memcpy(&v, Buf.constData() + off, size);
    return v;
}

DisasmLine DisasmView::decodeAt(u32 a, const QSet<u32>& pool)
{
    const DebugSymbols::Symbol* sym = Bridge->symbols().lookup(a);
    bool thumb = ViewMode == mode_Thumb || (ViewMode == mode_Auto && Thumb);
    bool data = false;
    if (ViewMode == mode_Auto && sym)
    {
        if (sym->Kind == "function")
            thumb = sym->Thumb;
        else
            data = true;
    }
    u32 off = a - BufBase;
    int avail = off < (u32)Buf.size() ? Buf.size() - off : 0;
    const u8* ptr = avail ? (const u8*)Buf.constData() + off : nullptr;
    if ((data || pool.contains(a)) && (a & 3) == 0)
    {
        DisasmLine line {};
        line.Addr = a;
        line.Size = 4;
        line.Mnemonic = ".word";
        line.Kind = InstrKind::Data;
        if (avail >= 4)
        {
            memcpy(&line.Opcode, ptr, 4);
            line.Operands = SymStr("0x" + Hex(line.Opcode));
        }
        else
            line.Operands = "??";
        return line;
    }
    return Dis.decode(a, ptr, avail, thumb);
}

void DisasmView::layoutRows()
{
    Rows.clear();
    DebugSymbols& syms = Bridge->symbols();
    QSet<u32> pool;
    u32 span = Top - BufBase;
    if (span <= (u32)Buf.size())
        for (u32 off = 0; off < span;)
        {
            DisasmLine line = decodeAt(BufBase + off, pool);
            if (line.IsLiteral)
                pool.insert(line.Target);
            off += line.Size ? line.Size : 2;
        }
    u32 a = Top;
    int want = visibleLines() + 1;
    const DebugSymbols::Symbol* first = syms.lookup(a);
    if (first && first->Addr != a)
        Rows.push_back({true, first->Addr, {}, true});
    while ((int)Rows.size() < want)
    {
        const DebugSymbols::Symbol* sym = syms.lookup(a);
        if (sym && sym->Addr == a)
            Rows.push_back({true, a, {}, false});
        DisasmLine line = decodeAt(a, pool);
        if (line.IsLiteral)
            pool.insert(line.Target);
        Rows.push_back({false, a, line, false});
        a += line.Size ? line.Size : 2;
    }
}

QString DisasmView::operands(const DisasmLine& line) const
{
    if (line.HasTarget && !line.IsLiteral)
    {
        QString name = SymStr(Bridge->symbols().describe(line.Target, false));
        if (!name.isEmpty())
            return name;
    }
    return SymStr(line.Operands);
}

QString DisasmView::comment(const DisasmLine& line) const
{
    DebugSymbols& syms = Bridge->symbols();
    if (line.IsLiteral)
    {
        bool ok;
        u32 v = readBuf(line.Target, 4, ok);
        if (!ok)
            return QString();
        QString name = SymStr(syms.describe(v));
        return "=0x" + Hex(v) + (name.isEmpty() ? QString() : " " + name);
    }
    if (line.HasTarget)
    {
        QString name = SymStr(syms.describe(line.Target, false));
        return name.isEmpty() ? QString() : Hex(line.Target);
    }
    if (line.Mnemonic == ".word")
    {
        QString name = SymStr(syms.describe(line.Opcode));
        return name;
    }
    return QString();
}

void DisasmView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const QPalette& pal = palette();
    p.fillRect(viewport()->rect(), pal.base());
    QFontMetrics fm = fontMetrics();
    int lh = lineHeight();
    int cw = fm.horizontalAdvance('0');
    int xGutter = 0;
    int xAddr = cw * 2;
    int xOp = xAddr + cw * 10;
    int xMn = xOp + cw * 10;
    int xArgs = xMn + cw * 8;
    int xComment = xArgs + cw * 30;

    QColor pcColor = pal.highlight().color();
    pcColor.setAlpha(70);
    QColor selColor = pal.alternateBase().color();
    QColor labelColor = pal.link().color();
    QColor dim = pal.placeholderText().color();

    for (int i = 0; i < (int)Rows.size(); i++)
    {
        const Row& row = Rows[i];
        int y = i * lh;
        QRect rect(0, y, viewport()->width(), lh);
        if (row.Label)
        {
            p.setPen(labelColor);
            const DebugSymbols::Symbol* sym = Bridge->symbols().lookup(row.Addr);
            QString text = sym ? SymStr(Bridge->symbols().name(*sym)) + (row.Continued ? QString(":  (continued from %1)").arg(Hex(sym->Addr)) : ":") : QString();
            const DebugSymbols::Module* mod = Bridge->symbols().moduleOf(sym);
            if (mod && mod->Overlay >= 0)
                text += "    [" + SymStr(mod->Name) + "]";
            p.drawText(xAddr, y + fm.ascent(), text);
            continue;
        }
        bool isPC = PCValid && row.Addr == PC;
        if (isPC)
            p.fillRect(rect, pcColor);
        else if (row.Addr == Selected)
            p.fillRect(rect, selColor);
        if (row.Addr == Selected && isPC)
            p.fillRect(QRect(0, y, 3, lh), pal.highlight());

        if (Breaks.contains(row.Addr))
        {
            int d = lh - 6;
            QRect dot(xGutter + 3, y + 3, d, d);
            p.setRenderHint(QPainter::Antialiasing);
            if (Disabled.contains(row.Addr))
            {
                p.setPen(QPen(QColor(200, 40, 40), 1.5));
                p.setBrush(Qt::NoBrush);
            }
            else
            {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(200, 40, 40));
            }
            p.drawEllipse(dot);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setBrush(Qt::NoBrush);
        }
        if (isPC)
        {
            p.setPen(pal.text().color());
            p.drawText(xGutter + 2, y + fm.ascent(), ">");
        }

        const DisasmLine& l = row.Line;
        p.setPen(dim);
        p.drawText(xAddr, y + fm.ascent(), Hex(row.Addr));
        p.drawText(xOp, y + fm.ascent(), l.Size == 2 ? Hex(l.Opcode, 4) : Hex(l.Opcode));
        p.setPen(InstrColor(l.Kind, pal));
        p.drawText(xMn, y + fm.ascent(), SymStr(l.Mnemonic));
        QString args = operands(l);
        p.drawText(xArgs, y + fm.ascent(), args);
        QString c = comment(l);
        if (!c.isEmpty())
        {
            p.setPen(dim);
            p.drawText(std::max(xComment, xArgs + fm.horizontalAdvance(args) + cw * 2), y + fm.ascent(), "; " + c);
        }
    }
}

void DisasmView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    syncScroll();
    fetch();
}

int DisasmView::rowAt(int y) const
{
    int i = y / lineHeight();
    if (i < 0 || i >= (int)Rows.size())
        return -1;
    return i;
}

void DisasmView::mousePressEvent(QMouseEvent* event)
{
    int i = rowAt(event->pos().y());
    if (i < 0 || Rows[i].Label)
        return;
    u32 addr = Rows[i].Addr;
    if (event->button() == Qt::LeftButton && event->pos().x() < fontMetrics().horizontalAdvance('0') * 2)
        emit toggleBreakpoint(addr);
    select(addr);
    viewport()->update();
}

void DisasmView::mouseDoubleClickEvent(QMouseEvent* event)
{
    int i = rowAt(event->pos().y());
    if (i < 0 || Rows[i].Label)
        return;
    const DisasmLine& l = Rows[i].Line;
    if (event->pos().x() < fontMetrics().horizontalAdvance('0') * 2)
        return;
    if (l.HasTarget && !l.IsLiteral)
        goTo(l.Target);
    else if (l.IsLiteral)
    {
        bool ok;
        u32 v = readBuf(l.Target, 4, ok);
        if (ok)
            goTo(v);
    }
}

void DisasmView::select(u32 addr)
{
    if (addr == Selected)
        return;
    Selected = addr;
    emit selectionChanged(addr);
}

void DisasmView::scrollLines(int lines)
{
    Top += lines * step();
    syncScroll();
    fetch();
}

void DisasmView::wheelEvent(QWheelEvent* event)
{
    int delta = event->angleDelta().y();
    scrollLines(delta > 0 ? -3 : 3);
}

void DisasmView::keyPressEvent(QKeyEvent* event)
{
    int sel = -1;
    for (int i = 0; i < (int)Rows.size(); i++)
        if (!Rows[i].Label && Rows[i].Addr == Selected)
            sel = i;
    switch (event->key())
    {
    case Qt::Key_Up:
        select(Selected - step());
        if (sel <= 1)
            scrollLines(-1);
        break;
    case Qt::Key_Down:
        select(Selected + (sel >= 0 ? Rows[sel].Line.Size : step()));
        if (sel >= visibleLines() - 2)
            scrollLines(1);
        break;
    case Qt::Key_PageUp:
        scrollLines(-visibleLines());
        break;
    case Qt::Key_PageDown:
        scrollLines(visibleLines());
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (sel >= 0)
        {
            const DisasmLine& l = Rows[sel].Line;
            if (l.HasTarget && !l.IsLiteral)
                goTo(l.Target);
        }
        break;
    case Qt::Key_Backspace:
        back();
        break;
    case Qt::Key_F9:
        emit toggleBreakpoint(Selected);
        break;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    viewport()->update();
}

void DisasmView::contextMenuEvent(QContextMenuEvent* event)
{
    int i = rowAt(event->pos().y());
    if (i >= 0 && !Rows[i].Label)
        select(Rows[i].Addr);
    viewport()->update();
    u32 addr = Selected;
    const DisasmLine* line = i >= 0 && !Rows[i].Label ? &Rows[i].Line : nullptr;

    QMenu menu(this);
    menu.addAction("Run to cursor", this, [this, addr] { emit runToCursor(addr); });
    menu.addAction("Toggle breakpoint", this, [this, addr] { emit toggleBreakpoint(addr); });
    QAction* setPC = menu.addAction("Set PC here", this, [this, addr] { emit setPCRequested(addr); });
    setPC->setEnabled(Bridge->isBroken() || !Bridge->isRunning());
    if (line && line->HasTarget)
    {
        u32 target = line->Target;
        if (line->IsLiteral)
        {
            bool ok;
            target = readBuf(line->Target, 4, ok);
        }
        menu.addAction("Follow " + Hex(target), this, [this, target] { goTo(target); });
    }
    menu.addAction("Back", this, &DisasmView::back)->setEnabled(!History.empty());
    menu.addSeparator();
    menu.addAction("Copy address", this, [addr] { QApplication::clipboard()->setText(Hex(addr)); });
    if (line)
    {
        QString text = Hex(addr) + "  " + SymStr(line->Mnemonic) + " " + operands(*line);
        menu.addAction("Copy line", this, [text] { QApplication::clipboard()->setText(text); });
    }
    menu.addSeparator();
    QMenu* modeMenu = menu.addMenu("Decode as");
    const char* names[] = {"Auto (symbols, CPSR)", "ARM", "Thumb"};
    for (int m = 0; m < 3; m++)
    {
        QAction* act = modeMenu->addAction(names[m], this, [this, m] { setMode((Mode)m); });
        act->setCheckable(true);
        act->setChecked(ViewMode == m);
    }
    menu.exec(event->globalPos());
}
