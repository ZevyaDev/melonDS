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

#include <QBuffer>
#include <QFileInfo>
#include <QImage>

#include "MediaWriters.h"

using namespace melonDS;

static const qint64 MaxSegmentBytes = 1900LL * 1000 * 1000;

static constexpr quint32 Tag(char a, char b, char c, char d)
{
    return (quint32)(quint8)a | ((quint32)(quint8)b << 8) | ((quint32)(quint8)c << 16) | ((quint32)(quint8)d << 24);
}

static void Put16(QByteArray& b, quint32 v)
{
    b.append((char)(v & 0xFF));
    b.append((char)((v >> 8) & 0xFF));
}

static void Put32(QByteArray& b, quint32 v)
{
    Put16(b, v & 0xFFFF);
    Put16(b, v >> 16);
}

static void PutTag(QByteArray& b, const char* t)
{
    b.append(t, 4);
}

static bool Patch32(QFile& f, qint64 pos, quint32 v)
{
    QByteArray b;
    Put32(b, v);
    qint64 end = f.pos();
    if (!f.seek(pos)) return false;
    if (f.write(b) != 4) return false;
    return f.seek(end);
}

static void WavHeader(QByteArray& h, int rate, int channels, quint32 dataBytes)
{
    PutTag(h, "RIFF");
    Put32(h, 36 + dataBytes);
    PutTag(h, "WAVE");
    PutTag(h, "fmt ");
    Put32(h, 16);
    Put16(h, 1);
    Put16(h, channels);
    Put32(h, rate);
    Put32(h, rate * channels * 2);
    Put16(h, channels * 2);
    Put16(h, 16);
    PutTag(h, "data");
    Put32(h, dataBytes);
}

bool WAVWriter::put(const char* data, qint64 size)
{
    if (File.write(data, size) != size)
    {
        Error = File.errorString();
        close();
        return false;
    }
    return true;
}

bool WAVWriter::open(const QString& path, int sampleRate, int channels)
{
    close();
    Error.clear();
    if (sampleRate <= 0 || channels <= 0)
    {
        Error = "invalid audio format";
        return false;
    }

    File.setFileName(path);
    if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        Error = File.errorString();
        return false;
    }

    SampleRate = sampleRate;
    Channels = channels;
    Frames = 0;
    FramesAtPatch = 0;

    QByteArray h;
    WavHeader(h, SampleRate, Channels, 0);
    return put(h.constData(), h.size());
}

void WAVWriter::patchHeader()
{
    quint32 dataBytes = (quint32)(Frames * Channels * 2);
    qint64 end = File.pos();
    QByteArray h;
    WavHeader(h, SampleRate, Channels, dataBytes);
    if (!File.seek(0) || File.write(h) != h.size() || !File.seek(end))
    {
        Error = File.errorString();
        File.close();
    }
    FramesAtPatch = Frames;
}

void WAVWriter::write(const s16* samples, size_t frames)
{
    if (!File.isOpen() || !frames) return;

    QByteArray b;
    b.resize((qsizetype)(frames * Channels * 2));
    char* p = b.data();
    for (size_t i = 0; i < frames * Channels; i++)
    {
        quint16 v = (quint16)samples[i];
        p[i * 2] = (char)(v & 0xFF);
        p[i * 2 + 1] = (char)(v >> 8);
    }

    if (!put(b.constData(), b.size())) return;
    Frames += frames;

    if (Frames - FramesAtPatch >= (quint64)SampleRate)
        patchHeader();
}

void WAVWriter::close()
{
    if (!File.isOpen()) return;
    patchHeader();
    File.close();
}

QString AVIWriter::segmentPath(int segment) const
{
    if (segment == 0) return BasePath;
    QFileInfo info(BasePath);
    QString suffix = info.suffix().isEmpty() ? QString() : "." + info.suffix();
    return info.path() + "/" + info.completeBaseName() + QString("_part%1").arg(segment + 1) + suffix;
}

void AVIWriter::fail(const QString& message)
{
    Error = message;
    if (File.isOpen()) File.close();
    Index.clear();
}

bool AVIWriter::put(const char* data, qint64 size)
{
    if (File.write(data, size) != size)
    {
        fail(File.errorString());
        return false;
    }
    return true;
}

bool AVIWriter::putChunk(quint32 id, const QByteArray& data)
{
    QByteArray h;
    Put32(h, id);
    Put32(h, (quint32)data.size());

    IndexEntry e;
    e.Id = id;
    e.Size = (quint32)data.size();
    e.Offset = (quint32)(File.pos() - MoviStart);

    if (!put(h.constData(), h.size())) return false;
    if (!put(data.constData(), data.size())) return false;
    if (data.size() & 1)
    {
        char z = 0;
        if (!put(&z, 1)) return false;
    }

    Index.append(e);
    return true;
}

bool AVIWriter::open(const QString& path, int width, int height, quint32 fpsNum, quint32 fpsDen, int sampleRate, AVICodec codec, int jpegQuality)
{
    close();
    Error.clear();
    if (width <= 0 || height <= 0 || !fpsNum || !fpsDen)
    {
        Error = "invalid video format";
        return false;
    }

    BasePath = path;
    Width = width;
    Height = height;
    FpsNum = fpsNum;
    FpsDen = fpsDen;
    SampleRate = sampleRate > 0 ? sampleRate : 0;
    Codec = codec;
    JpegQuality = jpegQuality;
    Segment = 0;
    TotalFrames = 0;

    return beginSegment();
}

bool AVIWriter::beginSegment()
{
    File.setFileName(segmentPath(Segment));
    if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        Error = File.errorString();
        return false;
    }

    SegFrames = 0;
    SegAudioFrames = 0;
    MaxFrameBytes = 0;
    Index.clear();

    bool audio = SampleRate > 0;
    quint32 frameBytes = (quint32)(((Width * 3 + 3) & ~3) * Height);
    quint32 usecPerFrame = (quint32)(1000000.0 * FpsDen / FpsNum + 0.5);
    quint32 fourcc = Codec == AVICodec::MJPEG ? Tag('M', 'J', 'P', 'G') : 0;

    QByteArray h;
    PutTag(h, "RIFF");
    Put32(h, 0);
    PutTag(h, "AVI ");

    PutTag(h, "LIST");
    Put32(h, 4 + 64 + 124 + (audio ? 102 : 0));
    PutTag(h, "hdrl");

    PutTag(h, "avih");
    Put32(h, 56);
    Put32(h, usecPerFrame);
    Put32(h, 0);
    Put32(h, 0);
    Put32(h, 0x10);
    PosAvihFrames = h.size();
    Put32(h, 0);
    Put32(h, 0);
    Put32(h, audio ? 2 : 1);
    PosAvihBuffer = h.size();
    Put32(h, frameBytes);
    Put32(h, Width);
    Put32(h, Height);
    for (int i = 0; i < 4; i++) Put32(h, 0);

    PutTag(h, "LIST");
    Put32(h, 4 + 64 + 48);
    PutTag(h, "strl");
    PutTag(h, "strh");
    Put32(h, 56);
    PutTag(h, "vids");
    Put32(h, fourcc);
    Put32(h, 0);
    Put16(h, 0);
    Put16(h, 0);
    Put32(h, 0);
    Put32(h, FpsDen);
    Put32(h, FpsNum);
    Put32(h, 0);
    PosVideoLength = h.size();
    Put32(h, 0);
    PosVideoBuffer = h.size();
    Put32(h, frameBytes);
    Put32(h, 0xFFFFFFFF);
    Put32(h, 0);
    Put16(h, 0);
    Put16(h, 0);
    Put16(h, Width);
    Put16(h, Height);
    PutTag(h, "strf");
    Put32(h, 40);
    Put32(h, 40);
    Put32(h, Width);
    Put32(h, Height);
    Put16(h, 1);
    Put16(h, 24);
    Put32(h, fourcc);
    Put32(h, frameBytes);
    Put32(h, 0);
    Put32(h, 0);
    Put32(h, 0);
    Put32(h, 0);

    if (audio)
    {
        PutTag(h, "LIST");
        Put32(h, 4 + 64 + 26);
        PutTag(h, "strl");
        PutTag(h, "strh");
        Put32(h, 56);
        PutTag(h, "auds");
        Put32(h, 0);
        Put32(h, 0);
        Put16(h, 0);
        Put16(h, 0);
        Put32(h, 0);
        Put32(h, 1);
        Put32(h, SampleRate);
        Put32(h, 0);
        PosAudioLength = h.size();
        Put32(h, 0);
        Put32(h, SampleRate * 4);
        Put32(h, 0xFFFFFFFF);
        Put32(h, 4);
        Put16(h, 0);
        Put16(h, 0);
        Put16(h, 0);
        Put16(h, 0);
        PutTag(h, "strf");
        Put32(h, 18);
        Put16(h, 1);
        Put16(h, 2);
        Put32(h, SampleRate);
        Put32(h, SampleRate * 4);
        Put16(h, 4);
        Put16(h, 16);
        Put16(h, 0);
    }

    PutTag(h, "LIST");
    Put32(h, 4);
    PutTag(h, "movi");

    MoviStart = h.size() - 4;
    return put(h.constData(), h.size());
}

bool AVIWriter::finishSegment()
{
    if (!File.isOpen()) return true;

    QByteArray idx;
    PutTag(idx, "idx1");
    Put32(idx, (quint32)Index.size() * 16);
    for (const IndexEntry& e : Index)
    {
        Put32(idx, e.Id);
        Put32(idx, 0x10);
        Put32(idx, e.Offset);
        Put32(idx, e.Size);
    }
    if (!put(idx.constData(), idx.size())) return false;

    qint64 end = File.pos();
    bool ok = Patch32(File, 4, (quint32)(end - 8))
        && Patch32(File, MoviStart - 4, (quint32)(end - idx.size() - MoviStart))
        && Patch32(File, PosAvihFrames, SegFrames)
        && Patch32(File, PosAvihBuffer, MaxFrameBytes)
        && Patch32(File, PosVideoLength, SegFrames)
        && Patch32(File, PosVideoBuffer, MaxFrameBytes);
    if (ok && SampleRate > 0)
        ok = Patch32(File, PosAudioLength, SegAudioFrames);
    if (!ok)
    {
        fail(File.errorString());
        return false;
    }

    File.close();
    Index.clear();
    return true;
}

bool AVIWriter::encodeFrame(const u32* pixels, QByteArray& out)
{
    if (Codec == AVICodec::MJPEG)
    {
        QImage img((const uchar*)pixels, Width, Height, Width * 4, QImage::Format_RGB32);
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "JPEG", JpegQuality))
        {
            Error = "JPEG encode failed";
            return false;
        }
        return true;
    }

    int stride = (Width * 3 + 3) & ~3;
    out.fill(0, stride * Height);
    uchar* dst = (uchar*)out.data();
    for (int y = 0; y < Height; y++)
    {
        const u32* src = pixels + (size_t)(Height - 1 - y) * Width;
        uchar* row = dst + (size_t)y * stride;
        for (int x = 0; x < Width; x++)
        {
            u32 p = src[x];
            row[x * 3] = (uchar)(p & 0xFF);
            row[x * 3 + 1] = (uchar)((p >> 8) & 0xFF);
            row[x * 3 + 2] = (uchar)((p >> 16) & 0xFF);
        }
    }
    return true;
}

bool AVIWriter::addFrame(const u32* pixels, const s16* audio, size_t audioFrames)
{
    if (!File.isOpen()) return false;

    QByteArray video;
    if (!encodeFrame(pixels, video))
    {
        fail(Error);
        return false;
    }

    QByteArray wave;
    if (SampleRate > 0 && audio && audioFrames)
    {
        wave.resize((qsizetype)(audioFrames * 4));
        char* p = wave.data();
        for (size_t i = 0; i < audioFrames * 2; i++)
        {
            quint16 v = (quint16)audio[i];
            p[i * 2] = (char)(v & 0xFF);
            p[i * 2 + 1] = (char)(v >> 8);
        }
    }
    else
        audioFrames = 0;

    qint64 projected = File.pos() + video.size() + wave.size() + 18
        + (qint64)(Index.size() + 3) * 16 + 8;
    if (SegFrames > 0 && projected > MaxSegmentBytes)
    {
        if (!finishSegment()) return false;
        Segment++;
        if (!beginSegment()) return false;
    }

    if (!putChunk(Tag('0', '0', 'd', 'c'), video)) return false;
    if (wave.size() && !putChunk(Tag('0', '1', 'w', 'b'), wave)) return false;

    if ((quint32)video.size() > MaxFrameBytes) MaxFrameBytes = (quint32)video.size();
    SegFrames++;
    SegAudioFrames += (quint32)audioFrames;
    TotalFrames++;
    return true;
}

void AVIWriter::close()
{
    if (File.isOpen()) finishSegment();
}
