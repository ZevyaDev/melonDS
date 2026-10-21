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

#include <algorithm>
#include <vector>

#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QUrl>
#include <QVBoxLayout>

#include "SourceView.h"
#include "DebugBridge.h"
#include "DebugTool.h"

using namespace melonDS;

namespace
{

constexpr int MaxRootDepth = 8;
constexpr int ContextLines = 2;
constexpr int HighlightAlpha = 45;

class CodeHighlighter : public QSyntaxHighlighter
{
public:
    explicit CodeHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {}

    void setPalette(const QPalette& pal)
    {
        bool dark = pal.base().color().lightness() < 128;
        auto format = [](const QColor& color) {
            QTextCharFormat f;
            f.setForeground(color);
            return f;
        };
        Rules.clear();
        auto words = [](const char* list) {
            return QRegularExpression(QString("\\b(%1)\\b").arg(QString(list).replace(' ', '|')));
        };
        Rules.push_back({words("alignas alignof asm auto break case catch class const constexpr const_cast continue default "
            "delete do dynamic_cast else enum explicit extern false for friend goto if inline mutable namespace new "
            "noexcept nullptr operator private protected public register reinterpret_cast return sizeof static "
            "static_assert static_cast struct switch template this throw true try typedef typeid typename union using "
            "virtual volatile while"),
            format(dark ? QColor(0x78, 0xA8, 0xF8) : QColor(0x21, 0x55, 0xC4))});
        Rules.push_back({words("void bool char short int long float double signed unsigned u8 u16 u32 u64 s8 s16 s32 s64 "
            "f32 f64 vu8 vu16 vu32 BOOL size_t fx16 fx32"),
            format(dark ? QColor(0x60, 0xC8, 0xE0) : QColor(0x00, 0x71, 0x8F))});
        Rules.push_back({QRegularExpression("\\b(0[xX][0-9A-Fa-f]+|\\d+(\\.\\d+)?)[uUlLfF]*\\b"),
            format(dark ? QColor(0xE0, 0xD0, 0x60) : QColor(0x8A, 0x73, 0x00))});
        Rules.push_back({QRegularExpression("^\\s*#.*$"),
            format(dark ? QColor(0xC8, 0xA0, 0xF8) : QColor(0x7B, 0x3F, 0xBF))});
        Rules.push_back({QRegularExpression("\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'"),
            format(dark ? QColor(0xF0, 0xA0, 0x50) : QColor(0xB0, 0x5A, 0x00))});
        Comment = format(pal.placeholderText().color());
        Rules.push_back({QRegularExpression("//.*$|^\\s*[@;].*$"), Comment});
        rehighlight();
    }

protected:
    void highlightBlock(const QString& text) override
    {
        for (const Rule& r : Rules)
        {
            QRegularExpressionMatchIterator it = r.Pattern.globalMatch(text);
            while (it.hasNext())
            {
                QRegularExpressionMatch m = it.next();
                setFormat((int)m.capturedStart(), (int)m.capturedLength(), r.Format);
            }
        }
        setCurrentBlockState(0);
        int start = previousBlockState() == 1 ? 0 : (int)text.indexOf("/*");
        int skip = previousBlockState() == 1 ? 0 : 2;
        while (start >= 0)
        {
            int end = (int)text.indexOf("*/", start + skip);
            int len = end < 0 ? (int)text.size() - start : end - start + 2;
            if (end < 0)
                setCurrentBlockState(1);
            setFormat(start, len, Comment);
            start = (int)text.indexOf("/*", start + len);
            skip = 2;
        }
    }

private:
    struct Rule
    {
        QRegularExpression Pattern;
        QTextCharFormat Format;
    };
    std::vector<Rule> Rules;
    QTextCharFormat Comment;
};

}

class CodeView : public QPlainTextEdit
{
public:
    explicit CodeView(QWidget* parent);

    std::function<void(int)> GutterClicked;
    QSet<int> Breaks;
    QSet<int> Disabled;
    int PCLine = -1;

    int gutterWidth() const;
    void paintGutter(QPaintEvent* event);
    void updateGutter() { Gutter->update(); }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* Gutter;
};

namespace
{

class GutterArea : public QWidget
{
public:
    explicit GutterArea(CodeView* view) : QWidget(view), View(view) {}
    QSize sizeHint() const override { return QSize(View->gutterWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* event) override { View->paintGutter(event); }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (View->GutterClicked)
            View->GutterClicked(View->cursorForPosition(QPoint(0, event->pos().y())).blockNumber());
    }

private:
    CodeView* View;
};

}

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent)
{
    Gutter = new GutterArea(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy)
            Gutter->scroll(0, dy);
        else
            Gutter->update(0, rect.y(), Gutter->width(), rect.height());
    });
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

int CodeView::gutterWidth() const
{
    int digits = (int)QString::number(std::max(1, blockCount())).size();
    return fontMetrics().height() + fontMetrics().horizontalAdvance('9') * digits + 8;
}

void CodeView::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    QRect cr = contentsRect();
    Gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void CodeView::paintGutter(QPaintEvent* event)
{
    QPainter p(Gutter);
    p.fillRect(event->rect(), palette().window());
    int lh = fontMetrics().height();
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            if (Breaks.contains(number))
            {
                p.setRenderHint(QPainter::Antialiasing);
                if (Disabled.contains(number))
                {
                    p.setPen(QPen(QColor(200, 40, 40), 1.5));
                    p.setBrush(Qt::NoBrush);
                }
                else
                {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(200, 40, 40));
                }
                p.drawEllipse(QRect(3, top + 3, lh - 6, lh - 6));
                p.setRenderHint(QPainter::Antialiasing, false);
                p.setBrush(Qt::NoBrush);
            }
            if (number == PCLine)
            {
                p.setPen(palette().text().color());
                p.drawText(QRect(0, top, lh, lh), Qt::AlignCenter, ">");
            }
            p.setPen(palette().placeholderText().color());
            p.drawText(QRect(lh, top, Gutter->width() - lh - 4, lh), Qt::AlignRight | Qt::AlignVCenter, QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

SourceView::SourceView(DebugBridge* bridge, QWidget* parent) : QWidget(parent), Bridge(bridge)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* top = new QHBoxLayout();
    Title = new QLabel(this);
    Title->setObjectName("SourceTitle");
    Title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Open = new QPushButton("Open", this);
    Open->setToolTip("Open the file: a local file in its editor, a GitHub file at these lines in the browser");
    Open->setEnabled(false);
    top->addWidget(Title, 1);
    top->addWidget(Open);
    layout->addLayout(top);
    Text = new CodeView(this);
    Text->setObjectName("SourceText");
    Text->setReadOnly(true);
    Text->setLineWrapMode(QPlainTextEdit::NoWrap);
    Text->setFont(MonoFont());
    Text->GutterClicked = [this](int line) { clickLine(line); };
    layout->addWidget(Text);
    auto* highlighter = new CodeHighlighter(Text->document());
    highlighter->setPalette(palette());
    Highlighter = highlighter;
    connect(Open, &QPushButton::clicked, this, [this] {
        if (!ShownLocation.isEmpty())
            QDesktopServices::openUrl(QUrl(DebugSource::BrowseUrl(ShownLocation, Shown.First, Shown.Last)));
    });
    message("Source", "Select a function in the listing to see its source.");
}

void SourceView::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        static_cast<CodeHighlighter*>(Highlighter)->setPalette(palette());
}

void SourceView::message(const QString& title, const QString& text)
{
    Title->setText(title);
    ShownLocation.clear();
    ShownRel.clear();
    Shown = {};
    ShownLine = -1;
    Text->Breaks.clear();
    Text->Disabled.clear();
    Text->PCLine = -1;
    Text->setExtraSelections({});
    Text->setPlainText(text);
    Open->setEnabled(false);
}

void SourceView::showAddress(u32 addr)
{
    DebugSymbols& syms = Bridge->symbols();
    auto set = syms.set();
    if (set != ForSet)
    {
        ForSet = set;
        Root.clear();
        RootFailed = false;
        Cache.clear();
        Key.clear();
    }
    if (syms.empty())
    {
        if (Key != "none")
            message("Source", "No symbols. Symbols... loads a dsd config.yaml from a file or a URL.");
        Key = "none";
        return;
    }

    const DebugSymbols::Symbol* sym = syms.lookup(addr);
    QString rel;
    int line = 0;
    bool haveFile = syms.lineAt(addr, rel, line) || syms.sourceFile(addr, rel);
    QString label = sym ? syms.name(*sym) : Hex(addr);
    QString key = rel + ':' + QString::number(line) + '\n' + (sym ? Hex(sym->Addr) + sym->Name : Hex(addr)) + (syms.demangle() ? "" : "\n");
    if (key == Key)
        return;
    Key = key;
    int request = ++Request;
    if (!sym && !haveFile)
    {
        message(label, "No function at " + label + ".");
        return;
    }
    if (!haveFile)
    {
        message(label, set->SourceFiles.isEmpty()
            ? "These symbols come without a delinks.txt, so there is no source to show."
            : label + " is not in any source file of this decomp yet.");
        return;
    }

    QString name = sym ? sym->Name : QString();
    QString pretty = sym ? sym->Pretty : QString();
    const void* owner = set.get();
    bool absolute = QDir::isAbsolutePath(rel);
    auto withRoot = [this, request, rel, name, pretty, label, owner, absolute, line] {
        if (request != Request || ForSet.get() != owner)
            return;
        if (Root.isEmpty() && !absolute)
        {
            message(label, "Cannot find " + rel + " in " + ForSet->Base + " or the folders above it.");
            return;
        }
        QString location = absolute ? rel : DebugSource::Join(Root, rel);
        if (Cache.contains(location))
        {
            display(location, rel, name, pretty, label, line);
            return;
        }
        Title->setText(rel + " (loading)");
        DebugSource::Read(location, this, [this, request, location, rel, name, pretty, label, owner, line](bool ok, const QByteArray& data) {
            if (ForSet.get() != owner)
                return;
            if (ok)
                Cache.insert(location, QString::fromUtf8(data));
            if (request != Request)
                return;
            if (!ok)
                message(label, "Cannot read " + location);
            else
                display(location, rel, name, pretty, label, line);
        });
    };
    if (!Root.isEmpty() || RootFailed || absolute)
    {
        withRoot();
        return;
    }
    QStringList candidates;
    for (QString dir = set->Base; !dir.isEmpty() && candidates.size() < MaxRootDepth; dir = DebugSource::Parent(dir))
        candidates << dir;
    Title->setText(rel + " (looking for the decomp root)");
    findRoot(rel, candidates, withRoot);
}

void SourceView::findRoot(const QString& rel, QStringList candidates, std::function<void()> then)
{
    if (candidates.isEmpty())
    {
        RootFailed = true;
        then();
        return;
    }
    QString dir = candidates.takeFirst();
    QString location = DebugSource::Join(dir, rel);
    if (!DebugSource::IsRemote(dir))
    {
        if (DebugSource::Exists(location))
        {
            Root = dir;
            then();
        }
        else
            findRoot(rel, candidates, then);
        return;
    }
    const void* owner = ForSet.get();
    DebugSource::Read(location, this, [this, rel, candidates, then, dir, location, owner](bool ok, const QByteArray& data) {
        if (ForSet.get() != owner)
            return;
        if (!ok)
        {
            findRoot(rel, candidates, then);
            return;
        }
        Root = dir;
        Cache.insert(location, QString::fromUtf8(data));
        then();
    });
}

void SourceView::display(const QString& location, const QString& rel, const QString& name, const QString& pretty, const QString& label, int line)
{
    const QString& text = Cache[location];
    if (location != ShownLocation)
    {
        ShownLocation = location;
        QString plain = text;
        plain.remove('\r');
        Text->setPlainText(plain);
    }
    ShownRel = rel;
    ShownLine = line - 1;
    Open->setEnabled(true);
    Shown = DebugSource::FindFunction(text, rel, name, pretty);
    applyMarks();
    if (Shown.First < 0 && ShownLine < 0)
    {
        Title->setText(rel + "   " + label + " (not found in the file)");
        Text->verticalScrollBar()->setValue(0);
        return;
    }
    if (Shown.First >= 0)
        Title->setText(QString("%1:%2-%3   %4").arg(rel).arg(Shown.First + 1).arg(Shown.Last + 1).arg(label)
            + (ShownLine >= 0 ? QString("   line %1").arg(line) : QString()));
    else
        Title->setText(QString("%1:%2   %3").arg(rel).arg(line).arg(label));
    int visible = std::max(1, Text->viewport()->height() / std::max(1, Text->fontMetrics().height()));
    int top = (Shown.First >= 0 ? Shown.First : ShownLine) - ContextLines;
    if (ShownLine >= 0 && (ShownLine < top || ShownLine >= top + visible - 1))
        top = ShownLine - visible / 2;
    Text->verticalScrollBar()->setValue(std::max(0, top));
}

void SourceView::applyMarks()
{
    Text->Breaks.clear();
    Text->Disabled.clear();
    Text->PCLine = -1;
    if (ShownLocation.isEmpty())
    {
        Text->updateGutter();
        return;
    }
    QList<QTextEdit::ExtraSelection> marks;
    auto mark = [&](int line, const QColor& color) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(color);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(Text->document()->findBlockByNumber(line));
        marks << sel;
    };
    QColor span = palette().highlight().color();
    span.setAlpha(HighlightAlpha);
    for (int line = Shown.First; line >= 0 && line <= Shown.Last; line++)
        mark(line, span);
    if (ShownLine >= 0)
    {
        QColor selected = palette().highlight().color();
        selected.setAlpha(HighlightAlpha * 2);
        mark(ShownLine, selected);
    }
    DebugSymbols& syms = Bridge->symbols();
    QString file;
    int line;
    if (Stopped && syms.lineAt(PC, file, line) && file == ShownRel)
    {
        Text->PCLine = line - 1;
        mark(Text->PCLine, QColor(255, 200, 0, 90));
    }
    for (u32 addr : Exec)
        if (syms.lineAt(addr, file, line) && file == ShownRel)
        {
            Text->Breaks.insert(line - 1);
            if (ExecDisabled.contains(addr))
                Text->Disabled.insert(line - 1);
        }
    Text->setExtraSelections(marks);
    Text->updateGutter();
}

void SourceView::setMarks(u32 pc, bool stopped, const QSet<u32>& exec, const QSet<u32>& disabled)
{
    PC = pc;
    Stopped = stopped;
    Exec = exec;
    ExecDisabled = disabled;
    applyMarks();
}

void SourceView::clickLine(int line)
{
    if (ShownRel.isEmpty())
        return;
    DebugSymbols& syms = Bridge->symbols();
    u32 addr;
    int module;
    if (syms.lineAddress(ShownRel, line + 1, addr, module))
    {
        emit toggleBreakpoint(addr);
        return;
    }
    auto set = syms.set();
    if (syms.lineSource().isEmpty())
        Title->setText("No line info: Symbols > Line info loads an ELF linked with -sym on");
    else if (module >= 0 && set)
        Title->setText(QString("%1:%2 is in %3, which is not loaded").arg(ShownRel).arg(line + 1).arg(set->Modules[module].Name));
    else
        Title->setText(QString("%1:%2 has no code").arg(ShownRel).arg(line + 1));
}

QString SourceView::describe() const
{
    QString out = Title->text();
    QString marks;
    if (ShownLine >= 0)
        marks += QString(" line=%1").arg(ShownLine + 1);
    if (Text->PCLine >= 0)
        marks += QString(" pc=%1").arg(Text->PCLine + 1);
    if (!Text->Breaks.isEmpty())
    {
        QList<int> lines = Text->Breaks.values();
        std::sort(lines.begin(), lines.end());
        QStringList list;
        for (int l : lines)
            list << QString::number(l + 1);
        marks += " breaks=" + list.join(',');
    }
    if (!marks.isEmpty())
        out += "\nmarks:" + marks;
    if (Shown.First < 0)
        return out + "\n" + Text->toPlainText().left(400);
    for (int line = Shown.First; line <= Shown.Last; line++)
        out += "\n" + Text->document()->findBlockByNumber(line).text();
    return out;
}
