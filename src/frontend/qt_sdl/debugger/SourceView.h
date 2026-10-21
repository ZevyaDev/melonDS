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

#ifndef SOURCEVIEW_H
#define SOURCEVIEW_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <memory>

#include "DebugSource.h"
#include "DebugSymbols.h"
#include "types.h"

class QLabel;
class QPushButton;
class QSyntaxHighlighter;
class DebugBridge;
class CodeView;

class SourceView : public QWidget
{
    Q_OBJECT

public:
    explicit SourceView(DebugBridge* bridge, QWidget* parent = nullptr);

    void showAddress(melonDS::u32 addr);
    void setMarks(melonDS::u32 pc, bool stopped, const QSet<melonDS::u32>& exec, const QSet<melonDS::u32>& disabled);
    void clickLine(int line);
    QString describe() const;

signals:
    void toggleBreakpoint(melonDS::u32 addr);

protected:
    void changeEvent(QEvent* event) override;

private:
    DebugBridge* Bridge;
    QLabel* Title;
    QPushButton* Open;
    CodeView* Text;
    QSyntaxHighlighter* Highlighter;

    std::shared_ptr<const DebugSymbols::Set> ForSet;
    QString Root;
    bool RootFailed = false;
    QHash<QString, QString> Cache;
    QString Key;
    QString ShownLocation;
    QString ShownRel;
    DebugSource::Span Shown;
    int ShownLine = -1;
    int Request = 0;

    melonDS::u32 PC = 0;
    bool Stopped = false;
    QSet<melonDS::u32> Exec;
    QSet<melonDS::u32> ExecDisabled;

    void message(const QString& title, const QString& text);
    void findRoot(const QString& rel, QStringList candidates, std::function<void()> then);
    void display(const QString& location, const QString& rel, const QString& name, const QString& pretty, const QString& label, int line);
    void applyMarks();
};

#endif
