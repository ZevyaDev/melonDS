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

#ifndef AUTOMATION_H
#define AUTOMATION_H

#include <QObject>
#include <QString>
#include <QStringList>

class EmuInstance;
class QTimer;

class Automation : public QObject
{
    Q_OBJECT

public:
    static void start(EmuInstance* inst);

private:
    Automation(EmuInstance* inst, const QString& dir);

    EmuInstance* Inst;
    QString Dir;
    QTimer* Timer;

    void poll();
    QString run(const QStringList& args);
};

#endif
