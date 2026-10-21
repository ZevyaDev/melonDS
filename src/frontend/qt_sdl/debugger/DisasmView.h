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

#ifndef DISASMVIEW_H
#define DISASMVIEW_H

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QSet>

#include <memory>
#include <vector>

#include "Disassembler.h"
#include "DebugBridge.h"

class DisasmView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum Mode
    {
        mode_Auto,
        mode_ARM,
        mode_Thumb,
    };

    DisasmView(DebugBridge* bridge, QWidget* parent = nullptr);

    void setCPU(int cpu);
    int cpu() const { return CPU; }
    void setMode(Mode mode);
    Mode mode() const { return ViewMode; }
    void setPC(melonDS::u32 pc, bool thumb, bool valid);
    void setBreakpoints(const QSet<melonDS::u32>& exec, const QSet<melonDS::u32>& disabled);
    void goTo(melonDS::u32 addr, bool record = true);
    void goToPC();
    void back();
    void reload();
    melonDS::u32 selected() const { return Selected; }

signals:
    void toggleBreakpoint(melonDS::u32 addr);
    void runToCursor(melonDS::u32 addr);
    void setPCRequested(melonDS::u32 addr);
    void addressHovered(melonDS::u32 addr);
    void modeChanged(int mode);
    void selectionChanged(melonDS::u32 addr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    struct Row
    {
        bool Label;
        melonDS::u32 Addr;
        DisasmLine Line;
        bool Continued;
    };

    DebugBridge* Bridge;
    Disassembler Dis;
    int CPU = 0;
    Mode ViewMode = mode_Auto;
    melonDS::u32 Top = 0x02000000;
    melonDS::u32 Selected = 0;
    melonDS::u32 PC = 0;
    bool PCThumb = false;
    bool PCValid = false;
    bool Thumb = false;
    QSet<melonDS::u32> Breaks;
    QSet<melonDS::u32> Disabled;
    std::vector<melonDS::u32> History;

    melonDS::u32 BufBase = 0;
    QByteArray Buf;
    int Request = 0;
    std::vector<Row> Rows;

    int lineHeight() const;
    int visibleLines() const;
    int step() const { return Thumb ? 2 : 4; }
    bool decideThumb() const;
    void fetch();
    void layoutRows();
    DisasmLine decodeAt(melonDS::u32 addr, const QSet<melonDS::u32>& pool);
    void syncScroll();
    void scrollLines(int lines);
    void select(melonDS::u32 addr);
    int rowAt(int y) const;
    melonDS::u32 readBuf(melonDS::u32 addr, int size, bool& ok) const;
    QString comment(const DisasmLine& line) const;
    QString operands(const DisasmLine& line) const;
};

#endif
