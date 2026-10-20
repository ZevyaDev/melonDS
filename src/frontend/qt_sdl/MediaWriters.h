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

#ifndef MEDIAWRITERS_H
#define MEDIAWRITERS_H

#include <QFile>
#include <QString>
#include <QVector>

#include "types.h"

class WAVWriter
{
public:
    bool open(const QString& path, int sampleRate, int channels);
    void write(const melonDS::s16* samples, size_t frames);
    void close();
    bool isOpen() const { return File.isOpen(); }
    quint64 frames() const { return Frames; }
    QString error() const { return Error; }

private:
    bool put(const char* data, qint64 size);
    void patchHeader();

    QFile File;
    QString Error;
    int Channels = 2;
    int SampleRate = 0;
    quint64 Frames = 0;
    quint64 FramesAtPatch = 0;
};

enum class AVICodec
{
    Uncompressed,
    MJPEG
};

class AVIWriter
{
public:
    bool open(const QString& path, int width, int height, quint32 fpsNum, quint32 fpsDen, int sampleRate, AVICodec codec, int jpegQuality = 90);
    bool addFrame(const melonDS::u32* pixels, const melonDS::s16* audio, size_t audioFrames);
    void close();
    bool isOpen() const { return File.isOpen(); }
    quint64 frames() const { return TotalFrames; }
    QString error() const { return Error; }

private:
    struct IndexEntry
    {
        quint32 Id;
        quint32 Size;
        quint32 Offset;
    };

    bool beginSegment();
    bool finishSegment();
    bool put(const char* data, qint64 size);
    bool putChunk(quint32 id, const QByteArray& data);
    bool encodeFrame(const melonDS::u32* pixels, QByteArray& out);
    QString segmentPath(int segment) const;
    void fail(const QString& message);

    QFile File;
    QString Error;
    QString BasePath;
    int Width = 0;
    int Height = 0;
    quint32 FpsNum = 0;
    quint32 FpsDen = 0;
    int SampleRate = 0;
    AVICodec Codec = AVICodec::Uncompressed;
    int JpegQuality = 90;

    int Segment = 0;
    quint64 TotalFrames = 0;
    quint32 SegFrames = 0;
    quint32 SegAudioFrames = 0;
    quint32 MaxFrameBytes = 0;
    qint64 MoviStart = 0;
    QVector<IndexEntry> Index;
    qint64 PosAvihFrames = 0;
    qint64 PosAvihBuffer = 0;
    qint64 PosVideoLength = 0;
    qint64 PosVideoBuffer = 0;
    qint64 PosAudioLength = 0;
};

#endif
