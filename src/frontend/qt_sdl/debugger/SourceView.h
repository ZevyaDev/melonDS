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
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <memory>

#include "DebugSource.h"
#include "DebugSymbols.h"
#include "types.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSyntaxHighlighter;
class DebugBridge;

class SourceView : public QWidget
{
    Q_OBJECT

public:
    explicit SourceView(DebugBridge* bridge, QWidget* parent = nullptr);

    void showAddress(melonDS::u32 addr);
    QString describe() const;

protected:
    void changeEvent(QEvent* event) override;

private:
    DebugBridge* Bridge;
    QLabel* Title;
    QPushButton* Open;
    QPlainTextEdit* Text;
    QSyntaxHighlighter* Highlighter;

    std::shared_ptr<const DebugSymbols::Set> ForSet;
    QString Root;
    bool RootFailed = false;
    QHash<QString, QString> Cache;
    QString Key;
    QString ShownLocation;
    DebugSource::Span Shown;
    int Request = 0;

    void message(const QString& title, const QString& text);
    void findRoot(const QString& rel, QStringList candidates, std::function<void()> then);
    void display(const QString& location, const QString& rel, const QString& name, const QString& pretty, const QString& label);
};

#endif
