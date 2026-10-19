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

#ifndef DEBUGTOOL_H
#define DEBUGTOOL_H

#include <QElapsedTimer>
#include <QFont>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "DebugBridge.h"

class DebugTool : public QWidget
{
    Q_OBJECT

public:
    DebugTool(DebugBridge* bridge, const QString& title, QWidget* parent = nullptr);

    DebugBridge* bridge() const { return Bridge; }

public slots:
    void requestRefresh();

protected:
    virtual void refresh() = 0;
    void setInterval(int ms);
    void setLiveRefresh(bool live) { Live = live; }

    template <typename Work, typename Done>
    void fetch(Work work, Done done)
    {
        if (Pending && Since.elapsed() < 2000)
            return;
        Pending = true;
        Since.restart();
        Bridge->call(this, std::move(work), [this, done = std::move(done)](auto& result) mutable {
            Pending = false;
            done(result);
        });
    }

    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    DebugBridge* Bridge;

private:
    QTimer* Timer;
    QElapsedTimer Since;
    bool Pending = false;
    bool Live = true;

    void onTimer();
};

QFont MonoFont();
QString Hex(melonDS::u32 value, int digits = 8);

#endif
