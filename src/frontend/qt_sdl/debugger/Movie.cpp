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

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

#include "Movie.h"
#include "CRC32.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Savestate.h"
#include "version.h"

using namespace melonDS;

constexpr u32 AutosaveFrames = 600;
constexpr u8 Cmd_Mic = 1;
constexpr u8 Cmd_Reset = 2;
constexpr u8 Cmd_Lid = 4;
static const char PadMnemonics[14] = "RLDUTSBAYXWEG";
static const int PadBitOfKey[12] = {5, 6, 7, 8, 12, 11, 9, 10, 1, 2, 3, 4};

enum RequestKind
{
    req_Record = 1,
    req_Play,
    req_Stop,
};

MovieRecord Movie::encode(const FrameInput& input)
{
    MovieRecord rec;
    for (int i = 0; i < 12; i++)
        if (input.Keys & (1 << i))
            rec.Pad |= 1 << PadBitOfKey[i];
    if (input.Lid)
        rec.Commands |= Cmd_Lid;
    if (input.Mic)
        rec.Commands |= Cmd_Mic;
    rec.Touch = input.Touching;
    rec.X = input.Touching ? input.TouchX : 0;
    rec.Y = input.Touching ? input.TouchY : 0;
    return rec;
}

void Movie::decode(const MovieRecord& rec, FrameInput& input)
{
    input.Keys = 0;
    for (int i = 0; i < 12; i++)
        if (rec.Pad & (1 << PadBitOfKey[i]))
            input.Keys |= 1 << i;
    input.Lid = rec.Commands & Cmd_Lid;
    input.Mic = rec.Commands & Cmd_Mic;
    input.Touching = rec.Touch;
    input.TouchX = rec.X;
    input.TouchY = rec.Y;
}

static bool SameRecord(const MovieRecord& a, const MovieRecord& b)
{
    return a.Commands == b.Commands && a.Pad == b.Pad && a.Touch == b.Touch && a.X == b.X && a.Y == b.Y;
}

static QByteArray RecordLine(const MovieRecord& rec)
{
    char pad[14];
    for (int bit = 0; bit < 13; bit++)
        pad[bit] = (rec.Pad & (1 << (12 - bit))) ? PadMnemonics[bit] : '.';
    pad[13] = 0;
    return QByteArray("|") + QByteArray::number(rec.Commands) + "|" + pad
        + QByteArray::number(rec.X).rightJustified(3, '0') + " "
        + QByteArray::number(rec.Y).rightJustified(3, '0') + " "
        + (rec.Touch ? "1" : "0") + " 000|\n";
}

static bool ParseRecord(const QByteArray& line, MovieRecord& rec)
{
    QList<QByteArray> parts = line.split('|');
    if (parts.size() < 3 || parts[2].size() < 13)
        return false;
    rec = MovieRecord();
    rec.Commands = parts[1].trimmed().toUInt();
    for (int i = 0; i < 13; i++)
    {
        rec.Pad <<= 1;
        char c = parts[2][i];
        if (c != '.' && c != ' ')
            rec.Pad |= 1;
    }
    QList<QByteArray> touch = parts[2].mid(13).simplified().split(' ');
    if (touch.size() >= 3)
    {
        rec.X = touch[0].toUInt();
        rec.Y = touch[1].toUInt();
        rec.Touch = touch[2].toUInt() != 0;
    }
    return true;
}

static QString SidecarPath(const std::string& file)
{
    return QString::fromStdString(file) + ".movie";
}

void Movie::requestRecord(const QString& path, const QString& author, bool powerOn)
{
    std::lock_guard lock(Lock);
    Requests.push_back({req_Record, path, author, powerOn, false});
}

void Movie::requestPlay(const QString& path, bool readOnly)
{
    std::lock_guard lock(Lock);
    Requests.push_back({req_Play, path, QString(), false, readOnly});
}

void Movie::requestStop()
{
    std::lock_guard lock(Lock);
    Requests.push_back({req_Stop});
}

void Movie::setReadOnly(bool readOnly)
{
    std::lock_guard lock(Lock);
    ReadOnly = readOnly;
    if (readOnly && State == Recording)
    {
        save();
        State = Playing;
    }
}

void Movie::setRerecords(u32 count)
{
    std::lock_guard lock(Lock);
    Rerecords = count;
    Dirty = true;
}

Movie::Status Movie::status() const
{
    std::lock_guard lock(Lock);
    Status s;
    s.State = State;
    s.ReadOnly = ReadOnly;
    s.Frame = Frame;
    s.Length = Records.size();
    s.Rerecords = Rerecords;
    s.Lag = Lag;
    s.Path = Path;
    s.Message = Message;
    return s;
}

bool Movie::active() const
{
    std::lock_guard lock(Lock);
    return State != Inactive;
}

bool Movie::playing() const
{
    std::lock_guard lock(Lock);
    return State == Playing;
}

void Movie::note(const QString& message)
{
    Message = message;
    Inst->osdAddMessage(0, "%s", message.toUtf8().constData());
}

bool Movie::captureState(NDS& nds)
{
    Savestate state;
    if (state.Error)
        return false;
    nds.DoSavestate(&state);
    if (state.Error)
        return false;
    StartState = qCompress(QByteArray((const char*)state.Buffer(), state.Length()), 9);
    return true;
}

bool Movie::applyState(NDS& nds)
{
    QByteArray raw = qUncompress(StartState);
    if (raw.isEmpty())
        return false;
    Savestate state(raw.data(), raw.size(), false);
    return nds.DoSavestate(&state) && !state.Error;
}

bool Movie::save()
{
    if (Path.isEmpty())
        return false;
    QSaveFile file(Path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QByteArray out;
    out += "version 1\n";
    out += "emuVersion melonDS " MELONDS_VERSION "\n";
    out += "rerecordCount " + QByteArray::number(Rerecords) + "\n";
    out += "romFilename " + RomName.toUtf8() + "\n";
    out += "romChecksum " + QByteArray::number(RomCRC, 16).toUpper().rightJustified(8, '0') + "\n";
    out += "romSerial " + RomSerial.toUtf8() + "\n";
    out += "guid " + Guid.toUtf8() + "\n";
    if (!Author.isEmpty())
        out += "comment author " + Author.toUtf8() + "\n";
    out += "savestate " + QByteArray(StartState.isEmpty() ? "0" : "1") + "\n";
    if (!StartState.isEmpty())
        out += "melonState " + StartState.toBase64() + "\n";
    file.write(out);
    for (const MovieRecord& rec : Records)
        file.write(RecordLine(rec));
    Dirty = false;
    return file.commit();
}

bool Movie::load(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = "cannot open " + path;
        return false;
    }
    Records.clear();
    StartState.clear();
    Guid.clear();
    Author.clear();
    RomName.clear();
    RomSerial.clear();
    RomCRC = 0;
    Rerecords = 0;
    while (!file.atEnd())
    {
        QByteArray line = file.readLine();
        while (line.endsWith('\n') || line.endsWith('\r'))
            line.chop(1);
        if (line.startsWith('|'))
        {
            MovieRecord rec;
            if (ParseRecord(line, rec))
                Records.push_back(rec);
            continue;
        }
        int space = line.indexOf(' ');
        QByteArray key = space < 0 ? line : line.left(space);
        QByteArray value = space < 0 ? QByteArray() : line.mid(space + 1);
        if (key == "rerecordCount")
            Rerecords = value.toUInt();
        else if (key == "guid")
            Guid = QString::fromUtf8(value);
        else if (key == "romFilename")
            RomName = QString::fromUtf8(value);
        else if (key == "romSerial")
            RomSerial = QString::fromUtf8(value);
        else if (key == "romChecksum")
            RomCRC = value.toUInt(nullptr, 16);
        else if (key == "comment" && value.startsWith("author "))
            Author = QString::fromUtf8(value.mid(7));
        else if (key == "melonState")
            StartState = QByteArray::fromBase64(value);
    }
    if (Guid.isEmpty())
        Guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return true;
}

static void RomIdentity(NDS& nds, QString& serial, u32& crc)
{
    const NDSCart::CartCommon* cart = nds.GetNDSCart();
    if (!cart)
        return;
    serial = QString::fromLatin1(cart->GetHeader().GameCode, 4);
    if (cart->GetROM())
        crc = CRC32(cart->GetROM(), cart->GetROMLength());
}

bool Movie::startRecording(const Request& req)
{
    if (req.PowerOn)
        Inst->reset();
    NDS* console = Inst->getNDS();
    if (!console || !captureState(*console) || !applyState(*console))
    {
        note("Movie: could not capture the start state");
        return false;
    }
    Path = req.Path;
    Author = req.Author;
    Guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    RomSerial.clear();
    RomCRC = 0;
    RomIdentity(*console, RomSerial, RomCRC);
    RomName = RomSerial;
    Records.clear();
    Frame = 0;
    Rerecords = 0;
    LagStart = console->NumLagFrames;
    Lag = 0;
    ReadOnly = false;
    ResetPending = false;
    State = Recording;
    if (!save())
    {
        State = Inactive;
        note("Movie: cannot write " + Path);
        return false;
    }
    note("Recording movie " + QFileInfo(Path).fileName());
    if (console->IsJITEnabled())
        note("JIT is on: a movie with rerecords can drift on playback; turn JIT off to rerecord");
    return true;
}

bool Movie::startPlayback(const Request& req)
{
    QString error;
    if (!load(req.Path, error))
    {
        note("Movie: " + error);
        return false;
    }
    Path = req.Path;
    if (StartState.isEmpty())
        Inst->reset();
    NDS* console = Inst->getNDS();
    if (!console)
        return false;
    if (!StartState.isEmpty() && !applyState(*console))
    {
        note("Movie: the start state does not load in this build");
        return false;
    }
    QString serial;
    u32 crc = 0;
    RomIdentity(*console, serial, crc);
    Frame = 0;
    LagStart = console->NumLagFrames;
    Lag = 0;
    ReadOnly = req.ReadOnly;
    ResetPending = false;
    State = Playing;
    if (!RomSerial.isEmpty() && serial != RomSerial)
        note("Movie was recorded on " + RomSerial + ", this is " + serial);
    else if (RomCRC && crc && crc != RomCRC)
        note("Movie ROM checksum differs from the loaded ROM");
    else
        note(QString("Playing movie %1 (%2 frames)").arg(QFileInfo(Path).fileName()).arg(Records.size()));
    return true;
}

void Movie::finish(const QString& message)
{
    if (State == Recording || Dirty)
        save();
    State = Inactive;
    note(message);
}

void Movie::process()
{
    std::vector<Request> requests;
    requests.swap(Requests);
    for (const Request& req : requests)
    {
        if (State != Inactive)
            finish("Movie stopped");
        if (req.Kind == req_Record)
            startRecording(req);
        else if (req.Kind == req_Play)
            startPlayback(req);
    }
}

void Movie::beforeFrame(FrameInput& input)
{
    std::lock_guard lock(Lock);
    if (!Inst->getNDS())
        return;
    if (!Requests.empty())
        process();
    if (State == Recording)
    {
        MovieRecord rec = encode(input);
        if (ResetPending)
            rec.Commands |= Cmd_Reset;
        ResetPending = false;
        Records.resize(Frame);
        Records.push_back(rec);
        Dirty = true;
    }
    else if (State == Playing)
    {
        if (Frame >= Records.size())
        {
            finish(QString("Movie finished (%1 frames)").arg(Records.size()));
            return;
        }
        const MovieRecord& rec = Records[Frame];
        if (rec.Commands & Cmd_Reset)
            Inst->reset();
        decode(rec, input);
    }
}

void Movie::afterFrame(NDS& nds)
{
    std::lock_guard lock(Lock);
    if (State == Inactive)
        return;
    Frame++;
    Lag = nds.NumLagFrames - LagStart;
    if (State == Recording && Frame % AutosaveFrames == 0)
        save();
}

void Movie::onReset()
{
    std::lock_guard lock(Lock);
    if (State == Recording)
        ResetPending = true;
}

bool Movie::snapshot(MovieSnapshot& out) const
{
    std::lock_guard lock(Lock);
    if (State == Inactive)
        return false;
    out.Guid = Guid;
    out.Frame = Frame;
    out.Records.assign(Records.begin(), Records.begin() + std::min<size_t>(Frame, Records.size()));
    return true;
}

void Movie::restore(NDS& nds, const MovieSnapshot* snap)
{
    std::lock_guard lock(Lock);
    if (State == Inactive)
        return;
    if (!snap || snap->Guid != Guid)
    {
        finish("Loaded state is not from this movie; movie stopped");
        return;
    }
    if (ReadOnly)
    {
        bool prefix = snap->Frame <= Records.size();
        for (u32 i = 0; prefix && i < snap->Frame && i < snap->Records.size(); i++)
            prefix = SameRecord(snap->Records[i], Records[i]);
        if (!prefix)
        {
            finish("Loaded state is on a different timeline; movie stopped");
            return;
        }
        Frame = snap->Frame;
        State = Playing;
    }
    else
    {
        Records = snap->Records;
        Frame = snap->Frame;
        Rerecords++;
        State = Recording;
        save();
        note(QString("Recording from frame %1 (rerecord %2)").arg(Frame).arg(Rerecords));
    }
    Lag = nds.NumLagFrames - LagStart;
}

void Movie::onStateSaved(const std::string& file)
{
    MovieSnapshot snap;
    QString sidecar = SidecarPath(file);
    if (!snapshot(snap))
    {
        if (QFile::exists(sidecar))
            QFile::remove(sidecar);
        return;
    }
    QSaveFile out(sidecar);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.write("guid " + snap.Guid.toUtf8() + "\n");
    out.write("frame " + QByteArray::number(snap.Frame) + "\n");
    for (const MovieRecord& rec : snap.Records)
        out.write(RecordLine(rec));
    out.commit();
}

void Movie::onStateLoaded(NDS& nds, const std::string& file)
{
    if (!active())
        return;
    QFile in(SidecarPath(file));
    if (!in.open(QIODevice::ReadOnly))
    {
        restore(nds, nullptr);
        return;
    }
    MovieSnapshot snap;
    while (!in.atEnd())
    {
        QByteArray line = in.readLine().trimmed();
        MovieRecord rec;
        if (line.startsWith('|'))
        {
            if (ParseRecord(line, rec))
                snap.Records.push_back(rec);
        }
        else if (line.startsWith("guid "))
            snap.Guid = QString::fromUtf8(line.mid(5));
        else if (line.startsWith("frame "))
            snap.Frame = line.mid(6).toUInt();
    }
    restore(nds, &snap);
}
