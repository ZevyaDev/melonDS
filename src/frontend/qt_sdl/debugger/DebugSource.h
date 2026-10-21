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

#ifndef DEBUGSOURCE_H
#define DEBUGSOURCE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;

namespace DebugSource
{

bool IsRemote(const QString& location);
QString Normalize(const QString& location);
QString DirOf(const QString& file);
QString Join(const QString& dir, const QString& relative);
QString Parent(const QString& dir);
bool Exists(const QString& location);
void ReadAll(const QStringList& locations, QHash<QString, QByteArray>& out);
void Read(const QString& location, QObject* context, std::function<void(bool ok, const QByteArray& data)> done);
QString BrowseUrl(const QString& location, int first, int last);

struct Span
{
    int First = -1;
    int Last = -1;
};

Span FindFunction(const QString& text, const QString& file, const QString& name, const QString& pretty);

}

#endif
