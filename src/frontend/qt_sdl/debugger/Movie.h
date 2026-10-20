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

#ifndef MOVIE_H
#define MOVIE_H

#include <mutex>
#include <string>
#include <vector>

#include <QByteArray>
#include <QString>

#include "types.h"

namespace melonDS { class NDS; }
class EmuInstance;

struct FrameInput
{
    melonDS::u16 Keys = 0;
    bool Touching = false;
    melonDS::u8 TouchX = 0;
    melonDS::u8 TouchY = 0;
    bool Lid = false;
    bool Mic = false;
};

struct MovieRecord
{
    melonDS::u8 Commands = 0;
    melonDS::u16 Pad = 0;
    melonDS::u8 X = 0;
    melonDS::u8 Y = 0;
    bool Touch = false;
};

struct MovieSnapshot
{
    QString Guid;
    melonDS::u32 Frame = 0;
    std::vector<MovieRecord> Records;
};

class Movie
{
public:
    enum Mode
    {
        Inactive,
        Recording,
        Playing,
    };

    struct Status
    {
        Mode State = Inactive;
        bool ReadOnly = true;
        melonDS::u32 Frame = 0;
        melonDS::u32 Length = 0;
        melonDS::u32 Rerecords = 0;
        melonDS::u32 Lag = 0;
        QString Path;
        QString Message;
    };

    explicit Movie(EmuInstance* inst) : Inst(inst) {}

    void requestRecord(const QString& path, const QString& author, bool powerOn);
    void requestPlay(const QString& path, bool readOnly);
    void requestStop();
    void setReadOnly(bool readOnly);
    void setRerecords(melonDS::u32 count);

    Status status() const;
    bool active() const;
    bool playing() const;

    void beforeFrame(FrameInput& input);
    void afterFrame(melonDS::NDS& nds);
    void onReset();
    void onStateSaved(const std::string& file);
    void onStateLoaded(melonDS::NDS& nds, const std::string& file);

    bool snapshot(MovieSnapshot& out) const;
    void restore(melonDS::NDS& nds, const MovieSnapshot* snap);

    static MovieRecord encode(const FrameInput& input);
    static void decode(const MovieRecord& rec, FrameInput& input);

private:
    struct Request
    {
        int Kind = 0;
        QString Path;
        QString Author;
        bool PowerOn = false;
        bool ReadOnly = true;
    };

    EmuInstance* Inst;
    mutable std::mutex Lock;
    Mode State = Inactive;
    bool ReadOnly = true;
    QString Path;
    QString Guid;
    QString Author;
    QString RomName;
    QString RomSerial;
    melonDS::u32 RomCRC = 0;
    QByteArray StartState;
    std::vector<MovieRecord> Records;
    melonDS::u32 Frame = 0;
    melonDS::u32 Rerecords = 0;
    melonDS::u32 LagStart = 0;
    melonDS::u32 Lag = 0;
    bool ResetPending = false;
    bool Dirty = false;
    QString Message;
    std::vector<Request> Requests;

    void process();
    bool startRecording(const Request& req);
    bool startPlayback(const Request& req);
    void finish(const QString& message);
    bool save();
    bool load(const QString& path, QString& error);
    bool captureState(melonDS::NDS& nds);
    bool applyState(melonDS::NDS& nds);
    void note(const QString& message);
};

#endif
