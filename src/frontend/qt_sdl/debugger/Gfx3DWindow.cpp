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
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Gfx3DWindow.h"
#include "GPU.h"
#include "GPU3D.h"

using namespace melonDS;

constexpr u32 TextureVRAMSize = 512 * 1024;
constexpr u32 PaletteVRAMSize = 128 * 1024;
constexpr u32 TextureKeyMask = 0x3FF0FFFF;
constexpr int MaxPreviewSize = 4096;

static const char* const TextureFormatNames[8] = {"none", "A3I5", "4-color", "16-color", "256-color", "4x4 compressed", "A5I3", "direct"};
static const int TextureBitsPerTexel[8] = {0, 8, 2, 4, 8, 2, 8, 16};
static const int PaletteSizes[8] = {0, 32, 4, 16, 256, 0, 8, 0};
static const char* const PolygonModeNames[4] = {"modulate", "decal", "toon/highlight", "shadow"};
static const char* const ShownFaceNames[4] = {"none", "back", "front", "both"};
static const char* const TexCoordModeNames[4] = {"none", "texcoord", "normal", "vertex"};

static u32 TextureFormat(u32 texParam)
{
    return (texParam >> 26) & 0x7;
}

static int TextureWidth(u32 texParam)
{
    return 8 << ((texParam >> 20) & 0x7);
}

static int TextureHeight(u32 texParam)
{
    return 8 << ((texParam >> 23) & 0x7);
}

static u32 TextureAddress(u32 texParam)
{
    return (texParam & 0xFFFF) << 3;
}

static u32 PaletteAddress(u32 texParam, u32 texPal)
{
    return TextureFormat(texParam) == 2 ? (texPal & 0x1FFF) << 3 : (texPal & 0x1FFF) << 4;
}

static u32 CompressedIndexAddress(u32 texAddr)
{
    return 0x20000 + ((texAddr & 0x1FFFC) >> 1) + (texAddr >= 0x40000 ? 0x10000 : 0);
}

static u64 TextureKey(const Gfx3DPolygon& poly)
{
    u32 format = TextureFormat(poly.TexParam);
    if (!format)
        return 0;
    u32 pal = format == 7 ? 0 : poly.TexPalette & 0x1FFF;
    return ((u64)(poly.TexParam & TextureKeyMask) << 32) | pal;
}

static QString WrapMode(u32 texParam, int repeatBit, int flipBit)
{
    if (!(texParam & (1u << repeatBit)))
        return "clamp";
    return (texParam & (1u << flipBit)) ? "flip" : "repeat";
}

static QString LightMask(u32 attr)
{
    QStringList on;
    for (int i = 0; i < 4; i++)
        if (attr & (1u << i))
            on << QString::number(i);
    return on.isEmpty() ? QString("none") : on.join(",");
}

static std::shared_ptr<const Gfx3DFrame> CaptureFrame(NDS& nds, bool withVRAM)
{
    auto frame = std::make_shared<Gfx3DFrame>();
    const melonDS::GPU& gpu = nds.GPU;
    const melonDS::GPU3D& gpu3d = gpu.GPU3D;
    u32 count = std::min<u32>(gpu3d.RenderNumPolygons, gpu3d.RenderPolygonRAM.size());
    frame->DispCnt = gpu3d.RenderDispCnt;
    frame->Polygons.resize(count);
    for (u32 i = 0; i < count; i++)
    {
        const melonDS::Polygon* src = gpu3d.RenderPolygonRAM[i];
        if (!src)
            continue;
        Gfx3DPolygon& dst = frame->Polygons[i];
        dst.Attr = src->Attr;
        dst.TexParam = src->TexParam;
        dst.TexPalette = src->TexPalette;
        dst.NumVertices = std::min<u32>(src->NumVertices, 10);
        dst.Type = src->Type;
        dst.WBuffer = src->WBuffer;
        dst.Degenerate = src->Degenerate;
        dst.FacingView = src->FacingView;
        dst.Translucent = src->Translucent;
        dst.IsShadowMask = src->IsShadowMask;
        dst.IsShadow = src->IsShadow;
        for (u32 v = 0; v < dst.NumVertices; v++)
        {
            const melonDS::Vertex* vtx = src->Vertices[v];
            if (!vtx)
                continue;
            Gfx3DVertex& out = dst.Vertices[v];
            out.X = vtx->FinalPosition[0];
            out.Y = vtx->FinalPosition[1];
            out.Z = src->FinalZ[v];
            out.W = src->FinalW[v];
            for (int c = 0; c < 4; c++)
                out.Clip[c] = vtx->Position[c];
            for (int c = 0; c < 3; c++)
                out.Color[c] = (u8)(vtx->FinalColor[c] >> 4);
            out.TexCoords[0] = vtx->TexCoords[0];
            out.TexCoords[1] = vtx->TexCoords[1];
            out.Clipped = vtx->Clipped;
        }
    }
    if (withVRAM)
    {
        frame->TextureVRAM.resize(TextureVRAMSize);
        frame->PaletteVRAM.resize(PaletteVRAMSize);
        for (u32 addr = 0; addr < TextureVRAMSize; addr += 8)
        {
            u64 value = gpu.ReadVRAM_Texture<u64>(addr);
            memcpy(&frame->TextureVRAM[addr], &value, 8);
        }
        for (u32 addr = 0; addr < PaletteVRAMSize; addr += 8)
        {
            u64 value = gpu.ReadVRAM_TexPal<u64>(addr);
            memcpy(&frame->PaletteVRAM[addr], &value, 8);
        }
    }
    return frame;
}

static bool SamePolygons(const Gfx3DFrame& a, const Gfx3DFrame& b)
{
    if (a.DispCnt != b.DispCnt || a.Polygons.size() != b.Polygons.size())
        return false;
    return a.Polygons.empty() || memcmp(a.Polygons.data(), b.Polygons.data(), a.Polygons.size() * sizeof(Gfx3DPolygon)) == 0;
}

struct TextureMemory
{
    const std::vector<u8>& Texture;
    const std::vector<u8>& Palette;

    u8 tex8(u32 addr) const { return Texture[addr & 0x7FFFF]; }
    u16 tex16(u32 addr) const { return Texture[addr & 0x7FFFE] | (Texture[(addr & 0x7FFFE) + 1] << 8); }
    u16 pal16(u32 addr) const { return Palette[addr & 0x1FFFE] | (Palette[(addr & 0x1FFFE) + 1] << 8); }
};

static u8 Expand5(u32 value)
{
    return (value << 3) | (value >> 2);
}

static QRgb ToRgb(u16 color, u8 alpha)
{
    return qRgba(Expand5(color & 0x1F), Expand5((color >> 5) & 0x1F), Expand5((color >> 10) & 0x1F), alpha * 255 / 31);
}

static u16 Mix555(u16 a, u16 b, u32 weightA, u32 weightB, u32 shift)
{
    u32 r = ((a & 0x001F) * weightA + (b & 0x001F) * weightB) >> shift;
    u32 g = (((a & 0x03E0) * weightA + (b & 0x03E0) * weightB) >> shift) & 0x03E0;
    u32 bl = (((a & 0x7C00) * weightA + (b & 0x7C00) * weightB) >> shift) & 0x7C00;
    return r | g | bl;
}

static u16 CompressedTexel(const TextureMemory& mem, u32 base, int width, u32 palBase, int s, int t, u8& alpha)
{
    u32 addr = (base + ((t & 0x3FC) * (width >> 2)) + (s & 0x3FC) + (t & 0x3)) & 0x7FFFF;
    u8 code = (addr >= 0x20000 && addr < 0x40000) ? 0 : (mem.tex8(addr) >> (2 * (s & 0x3))) & 0x3;
    u16 info = mem.tex16(CompressedIndexAddress(addr));
    u32 palAddr = palBase + ((info & 0x3FFF) << 2);
    u32 mode = info >> 14;
    u16 color0 = mem.pal16(palAddr);
    u16 color1 = mem.pal16(palAddr + 2);
    alpha = 31;
    switch (code)
    {
    case 0:
        return color0;
    case 1:
        return color1;
    case 2:
        if (mode == 1)
            return Mix555(color0, color1, 1, 1, 1);
        if (mode == 3)
            return Mix555(color0, color1, 5, 3, 3);
        return mem.pal16(palAddr + 4);
    default:
        if (mode == 2)
            return mem.pal16(palAddr + 6);
        if (mode == 3)
            return Mix555(color0, color1, 3, 5, 3);
        alpha = 0;
        return 0;
    }
}

static void TexelAt(const TextureMemory& mem, u32 texParam, u32 texPal, int s, int t, u16& color, u8& alpha)
{
    u32 base = TextureAddress(texParam);
    int width = TextureWidth(texParam);
    u8 alpha0 = (texParam & (1u << 29)) ? 0 : 31;
    u32 pal = texPal & 0x1FFF;
    color = 0;
    alpha = 31;
    switch (TextureFormat(texParam))
    {
    case 1:
        {
            u8 pixel = mem.tex8(base + t * width + s);
            color = mem.pal16((pal << 4) + ((pixel & 0x1F) << 1));
            alpha = ((pixel >> 3) & 0x1C) + (pixel >> 6);
        }
        break;
    case 2:
        {
            u8 pixel = (mem.tex8(base + ((t * width + s) >> 2)) >> ((s & 0x3) << 1)) & 0x3;
            color = mem.pal16((pal << 3) + (pixel << 1));
            alpha = pixel ? 31 : alpha0;
        }
        break;
    case 3:
        {
            u8 pixel = mem.tex8(base + ((t * width + s) >> 1));
            pixel = (s & 0x1) ? pixel >> 4 : pixel & 0xF;
            color = mem.pal16((pal << 4) + (pixel << 1));
            alpha = pixel ? 31 : alpha0;
        }
        break;
    case 4:
        {
            u8 pixel = mem.tex8(base + t * width + s);
            color = mem.pal16((pal << 4) + (pixel << 1));
            alpha = pixel ? 31 : alpha0;
        }
        break;
    case 5:
        color = CompressedTexel(mem, base, width, pal << 4, s, t, alpha);
        break;
    case 6:
        {
            u8 pixel = mem.tex8(base + t * width + s);
            color = mem.pal16((pal << 4) + ((pixel & 0x7) << 1));
            alpha = pixel >> 3;
        }
        break;
    case 7:
        color = mem.tex16(base + ((t * width + s) << 1));
        alpha = (color & 0x8000) ? 31 : 0;
        break;
    }
}

static QImage DecodeTexture(const Gfx3DFrame& vram, u32 texParam, u32 texPal, bool ignoreAlpha)
{
    TextureMemory mem {vram.TextureVRAM, vram.PaletteVRAM};
    int width = TextureWidth(texParam);
    int height = TextureHeight(texParam);
    QImage image(width, height, QImage::Format_ARGB32);
    for (int t = 0; t < height; t++)
    {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(t));
        for (int s = 0; s < width; s++)
        {
            u16 color;
            u8 alpha;
            TexelAt(mem, texParam, texPal, s, t, color, alpha);
            line[s] = ToRgb(color, ignoreAlpha ? 31 : alpha);
        }
    }
    return image;
}

static QPixmap PaletteStrip(const Gfx3DFrame& vram, u32 texParam, u32 texPal)
{
    int count = PaletteSizes[TextureFormat(texParam)];
    if (!count)
        return QPixmap();
    constexpr int Cell = 12;
    constexpr int PerRow = 16;
    TextureMemory mem {vram.TextureVRAM, vram.PaletteVRAM};
    int columns = std::min(count, PerRow);
    int rows = (count + PerRow - 1) / PerRow;
    QImage image(columns * Cell, rows * Cell, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter p(&image);
    u32 base = PaletteAddress(texParam, texPal);
    for (int i = 0; i < count; i++)
        p.fillRect((i % PerRow) * Cell, (i / PerRow) * Cell, Cell - 1, Cell - 1, QColor(ToRgb(mem.pal16(base + i * 2), 31)));
    p.end();
    return QPixmap::fromImage(image);
}

static QString DescribeTexture(u32 texParam, u32 texPal)
{
    u32 format = TextureFormat(texParam);
    int width = TextureWidth(texParam);
    int height = TextureHeight(texParam);
    u32 addr = TextureAddress(texParam);
    u32 bytes = width * height * TextureBitsPerTexel[format] / 8;
    QStringList lines;
    lines << QString("%1 %2x%3, texels %4-%5 (0x%6 bytes)").arg(TextureFormatNames[format]).arg(width).arg(height)
        .arg(Hex(addr, 5)).arg(Hex(addr + bytes - 1, 5)).arg(bytes, 0, 16);
    if (format == 5)
        lines << QString("block palette indices %1 (0x%2 bytes)").arg(Hex(CompressedIndexAddress(addr), 5)).arg(bytes / 2, 0, 16);
    if (format != 0 && format != 7)
    {
        QString colors = PaletteSizes[format] ? QString(", %1 colors").arg(PaletteSizes[format]) : QString();
        lines << QString("palette %1 (PLTT_BASE %2)%3").arg(Hex(PaletteAddress(texParam, texPal), 5)).arg(Hex(texPal & 0x1FFF, 4)).arg(colors);
    }
    lines << QString("S %1, T %2, color 0 %3, texcoord transform %4").arg(WrapMode(texParam, 16, 18)).arg(WrapMode(texParam, 17, 19))
        .arg((texParam & (1u << 29)) ? "transparent" : "opaque").arg(TexCoordModeNames[texParam >> 30]);
    return lines.join('\n');
}

static QString DescribePolygon(int index, const Gfx3DPolygon& poly)
{
    u32 attr = poly.Attr;
    u32 alpha = (attr >> 16) & 0x1F;
    QStringList flags;
    if (poly.Translucent)
        flags << "translucent";
    if (poly.IsShadowMask)
        flags << "shadow mask";
    if (poly.IsShadow)
        flags << "shadow";
    if (poly.Degenerate)
        flags << "degenerate";
    if (poly.Type == 1)
        flags << "line";
    flags << (poly.FacingView ? "front-facing" : "back-facing");
    flags << (poly.WBuffer ? "W-buffer" : "Z-buffer");
    QStringList lines;
    lines << QString("Polygon %1, %2 vertices: %3").arg(index).arg(poly.NumVertices).arg(flags.join(", "));
    lines << QString("POLYGON_ATTR %1: %2, lights %3, show %4, alpha %5%6, ID %7").arg(Hex(attr))
        .arg(PolygonModeNames[(attr >> 4) & 0x3]).arg(LightMask(attr)).arg(ShownFaceNames[(attr >> 6) & 0x3])
        .arg(alpha).arg(alpha ? "" : " (wireframe)").arg((attr >> 24) & 0x3F);
    lines << QString("depth test %1, translucent depth %2, far plane %3, 1-dot %4, fog %5")
        .arg((attr & (1u << 14)) ? "equal" : "less").arg((attr & (1u << 11)) ? "update" : "keep")
        .arg((attr & (1u << 12)) ? "clip" : "hide").arg((attr & (1u << 13)) ? "render" : "hide")
        .arg((attr & (1u << 15)) ? "on" : "off");
    if (TextureFormat(poly.TexParam))
        lines << QString("TEXIMAGE_PARAM %1: ").arg(Hex(poly.TexParam)) + DescribeTexture(poly.TexParam, poly.TexPalette).replace('\n', "; ");
    else
        lines << QString("TEXIMAGE_PARAM %1: no texture").arg(Hex(poly.TexParam));
    return lines.join('\n');
}

static QString DescribeDispCnt(u32 cnt)
{
    QStringList on;
    if (cnt & (1u << 0))
        on << "textures";
    on << ((cnt & (1u << 1)) ? "highlight" : "toon");
    if (cnt & (1u << 2))
        on << "alpha test";
    if (cnt & (1u << 3))
        on << "blending";
    if (cnt & (1u << 4))
        on << "anti-aliasing";
    if (cnt & (1u << 5))
        on << "edge marking";
    if (cnt & (1u << 7))
        on << QString("fog (%1, shift %2)").arg((cnt & (1u << 6)) ? "alpha only" : "color and alpha").arg((cnt >> 8) & 0xF);
    if (cnt & (1u << 13))
        on << "RAM overflow";
    if (cnt & (1u << 14))
        on << "rear-plane bitmap";
    return QString("DISP3DCNT %1: %2").arg(Hex(cnt, 4)).arg(on.join(", "));
}

static QString DescribeFrame(const Gfx3DFrame& frame)
{
    int translucent = 0;
    for (const Gfx3DPolygon& poly : frame.Polygons)
        if (poly.Translucent)
            translucent++;
    QString buffer;
    if (!frame.Polygons.empty())
        buffer = frame.Polygons[0].WBuffer ? ", W-buffer" : ", Z-buffer";
    return QString("%1 polygons (%2 translucent)%3 | %4").arg(int(frame.Polygons.size())).arg(translucent).arg(buffer).arg(DescribeDispCnt(frame.DispCnt));
}

static QStringList PolygonColumns(int index, const Gfx3DPolygon& poly)
{
    u32 attr = poly.Attr;
    u32 format = TextureFormat(poly.TexParam);
    bool textured = format != 0;
    return {
        QString::number(index),
        QString::number(poly.NumVertices) + (poly.Type == 1 ? " line" : ""),
        poly.IsShadowMask ? "shadow mask" : PolygonModeNames[(attr >> 4) & 0x3],
        QString::number((attr >> 16) & 0x1F),
        QString::number((attr >> 24) & 0x3F),
        ShownFaceNames[(attr >> 6) & 0x3],
        poly.FacingView ? "front" : "back",
        poly.Translucent ? "yes" : "",
        (attr & (1u << 15)) ? "on" : "",
        QString((attr & (1u << 14)) ? "equal" : "less") + ((attr & (1u << 11)) ? " +set" : ""),
        TextureFormatNames[format],
        textured ? Hex(TextureAddress(poly.TexParam), 5) : QString(),
        textured ? QString("%1x%2").arg(TextureWidth(poly.TexParam)).arg(TextureHeight(poly.TexParam)) : QString(),
        textured ? WrapMode(poly.TexParam, 16, 18) + "/" + WrapMode(poly.TexParam, 17, 19) : QString(),
        textured && format != 7 ? Hex(PaletteAddress(poly.TexParam, poly.TexPalette), 5) : QString(),
    };
}

static QStringList VertexColumns(int index, const Gfx3DVertex& v)
{
    return {
        QString::number(index),
        QString::number(v.X),
        QString::number(v.Y),
        Hex(v.Z, 6),
        Hex(v.W, 4),
        QString("%1,%2,%3").arg(v.Color[0]).arg(v.Color[1]).arg(v.Color[2]),
        QString::number(v.TexCoords[0] / 16.0, 'f', 2),
        QString::number(v.TexCoords[1] / 16.0, 'f', 2),
        QString::number(v.Clip[0] / 4096.0, 'f', 3),
        QString::number(v.Clip[1] / 4096.0, 'f', 3),
        QString::number(v.Clip[2] / 4096.0, 'f', 3),
        QString::number(v.Clip[3] / 4096.0, 'f', 3),
        v.Clipped ? "yes" : "",
    };
}

static double SegmentDistance(const Gfx3DVertex& a, const Gfx3DVertex& b, double x, double y)
{
    double dx = b.X - a.X;
    double dy = b.Y - a.Y;
    double length = dx * dx + dy * dy;
    double t = length > 0 ? std::clamp(((x - a.X) * dx + (y - a.Y) * dy) / length, 0.0, 1.0) : 0.0;
    double px = a.X + t * dx - x;
    double py = a.Y + t * dy - y;
    return std::sqrt(px * px + py * py);
}

static bool DepthAt(const Gfx3DPolygon& poly, double x, double y, double& depth)
{
    int n = poly.NumVertices;
    const Gfx3DVertex* v = poly.Vertices;
    bool hasArea = false;
    for (int k = 1; k + 1 < n; k++)
    {
        const Gfx3DVertex& a = v[0];
        const Gfx3DVertex& b = v[k];
        const Gfx3DVertex& c = v[k + 1];
        double area = double(b.X - a.X) * (c.Y - a.Y) - double(c.X - a.X) * (b.Y - a.Y);
        if (area == 0)
            continue;
        hasArea = true;
        double wa = ((b.X - x) * (c.Y - y) - (c.X - x) * (b.Y - y)) / area;
        double wb = ((c.X - x) * (a.Y - y) - (a.X - x) * (c.Y - y)) / area;
        double wc = 1.0 - wa - wb;
        if (wa >= 0 && wb >= 0 && wc >= 0)
        {
            depth = wa * a.Z + wb * b.Z + wc * c.Z;
            return true;
        }
    }
    if (hasArea)
        return false;
    for (int k = 0; k < n; k++)
    {
        const Gfx3DVertex& a = v[k];
        const Gfx3DVertex& b = v[(k + 1) % n];
        if (SegmentDistance(a, b, x, y) <= 1.5)
        {
            depth = std::min(a.Z, b.Z);
            return true;
        }
    }
    return false;
}

static int PickPolygon(const std::vector<Gfx3DPolygon>& polys, QPointF point)
{
    int best = -1;
    double bestDepth = 0;
    bool bestIsMask = true;
    for (size_t i = 0; i < polys.size(); i++)
    {
        double depth;
        if (!DepthAt(polys[i], point.x(), point.y(), depth))
            continue;
        bool mask = polys[i].IsShadowMask;
        if (best < 0 || (bestIsMask && !mask) || (mask == bestIsMask && depth <= bestDepth))
        {
            best = i;
            bestDepth = depth;
            bestIsMask = mask;
        }
    }
    return best;
}

static QPolygonF Outline(const Gfx3DPolygon& poly, const QRectF& screen)
{
    double scale = screen.width() / 256.0;
    QPolygonF shape;
    for (u32 i = 0; i < poly.NumVertices; i++)
        shape << QPointF(screen.left() + poly.Vertices[i].X * scale, screen.top() + poly.Vertices[i].Y * scale);
    return shape;
}

static QColor OutlineColor(const Gfx3DPolygon& poly)
{
    if (poly.IsShadowMask || poly.IsShadow)
        return QColor(200, 90, 220);
    if (poly.Translucent)
        return QColor(90, 170, 255);
    return QColor(170, 170, 170);
}

static void DrawShape(QPainter& p, const QPolygonF& shape)
{
    if (shape.size() >= 3)
        p.drawPolygon(shape);
    else if (shape.size() == 2)
        p.drawLine(shape[0], shape[1]);
    else if (shape.size() == 1)
        p.drawPoint(shape[0]);
}

class Gfx3DWireframe : public QWidget
{
public:
    explicit Gfx3DWireframe(QWidget* parent) : QWidget(parent)
    {
        setMinimumSize(256, 192);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    std::function<void(int)> Picked;

    void setFrame(std::shared_ptr<const Gfx3DFrame> frame)
    {
        Frame = std::move(frame);
        update();
    }

    void setSelected(int index)
    {
        Selected = index;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().dark());
        QRectF screen = screenRect();
        p.fillRect(screen, Qt::black);
        if (!Frame)
            return;
        p.setClipRect(screen);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::NoBrush);
        const std::vector<Gfx3DPolygon>& polys = Frame->Polygons;
        for (size_t i = 0; i < polys.size(); i++)
        {
            if ((int)i == Selected)
                continue;
            QPen pen(OutlineColor(polys[i]));
            pen.setCosmetic(true);
            p.setPen(pen);
            DrawShape(p, Outline(polys[i], screen));
        }
        if (Selected >= 0 && Selected < (int)polys.size())
        {
            QPen pen(QColor(255, 210, 0), 2);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(QColor(255, 210, 0, 80));
            DrawShape(p, Outline(polys[Selected], screen));
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        QRectF screen = screenRect();
        QPointF pos = event->position();
        if (!Frame || !screen.contains(pos))
            return;
        double scale = screen.width() / 256.0;
        int index = PickPolygon(Frame->Polygons, QPointF((pos.x() - screen.left()) / scale, (pos.y() - screen.top()) / scale));
        if (Picked)
            Picked(index);
    }

private:
    std::shared_ptr<const Gfx3DFrame> Frame;
    int Selected = -1;

    QRectF screenRect() const
    {
        double scale = std::min(width() / 256.0, height() / 192.0);
        double w = 256 * scale;
        double h = 192 * scale;
        return QRectF((width() - w) / 2, (height() - h) / 2, w, h);
    }
};

class Gfx3DTextureView : public QWidget
{
public:
    explicit Gfx3DTextureView(QWidget* parent) : QWidget(parent)
    {
        QPixmap checker(16, 16);
        checker.fill(QColor(200, 200, 200));
        QPainter p(&checker);
        p.fillRect(0, 0, 8, 8, QColor(150, 150, 150));
        p.fillRect(8, 8, 8, 8, QColor(150, 150, 150));
        p.end();
        Checker = QBrush(checker);
        setFixedSize(1, 1);
    }

    void setImage(const QImage& image, int scale)
    {
        Image = image;
        Scale = scale;
        setFixedSize(image.isNull() ? QSize(1, 1) : image.size() * scale);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        if (Image.isNull())
            return;
        QPainter p(this);
        QRect exposed = event->rect();
        p.fillRect(exposed, Checker);
        QRect source(exposed.left() / Scale, exposed.top() / Scale, exposed.width() / Scale + 2, exposed.height() / Scale + 2);
        source = source.intersected(Image.rect());
        p.drawImage(QRect(source.topLeft() * Scale, source.size() * Scale), Image, source);
    }

private:
    QImage Image;
    QBrush Checker;
    int Scale = 1;
};

static QTreeWidget* MakeTree(QWidget* parent, const QStringList& headers)
{
    QTreeWidget* tree = new QTreeWidget(parent);
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAllColumnsShowFocus(true);
    tree->setFont(MonoFont());
    return tree;
}

static QLabel* MakeDetail(QWidget* parent)
{
    QLabel* label = new QLabel(parent);
    label->setFont(MonoFont());
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

Gfx3DWindow::Gfx3DWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "3D graphics", parent)
{
    resize(1000, 700);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    LiveUpdate = new QCheckBox("Live", this);
    QPushButton* reload = new QPushButton("Refresh", this);
    row->addWidget(LiveUpdate);
    row->addWidget(reload);
    row->addStretch();
    layout->addLayout(row);

    Tabs = new QTabWidget(this);

    QSplitter* polygonPage = new QSplitter(Qt::Vertical, this);
    PolygonTree = MakeTree(polygonPage, {"#", "Verts", "Mode", "Alpha", "ID", "Show", "Face", "Trans", "Fog", "Depth", "Texture", "Tex addr", "Size", "Wrap S/T", "Palette"});
    QWidget* lower = new QWidget(polygonPage);
    QVBoxLayout* lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    VertexTree = MakeTree(lower, {"#", "X", "Y", "Z", "W", "Color", "S", "T", "Clip X", "Clip Y", "Clip Z", "Clip W", "Clipped"});
    PolygonDetail = MakeDetail(lower);
    lowerLayout->addWidget(VertexTree, 1);
    lowerLayout->addWidget(PolygonDetail);
    polygonPage->setStretchFactor(0, 3);
    polygonPage->setStretchFactor(1, 2);
    Tabs->addTab(polygonPage, "Polygons");

    QWidget* wirePage = new QWidget(this);
    QVBoxLayout* wireLayout = new QVBoxLayout(wirePage);
    Wireframe = new Gfx3DWireframe(wirePage);
    wireLayout->addWidget(Wireframe, 1);
    wireLayout->addWidget(new QLabel("Gray: opaque, blue: translucent, purple: shadow, yellow: selected. Click picks the nearest polygon under the cursor.", wirePage));
    Tabs->addTab(wirePage, "Wireframe");

    TexturesPage = new QSplitter(Qt::Horizontal, this);
    TextureTree = MakeTree(TexturesPage, {"Address", "Format", "Size", "Palette", "Polygons"});
    TextureTree->setSortingEnabled(true);
    TextureTree->sortByColumn(0, Qt::AscendingOrder);
    QWidget* preview = new QWidget(TexturesPage);
    QVBoxLayout* previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* controls = new QHBoxLayout();
    Zoom = new QSpinBox(preview);
    Zoom->setRange(1, 16);
    Zoom->setValue(2);
    Zoom->setSuffix("x");
    IgnoreAlpha = new QCheckBox("Ignore alpha", preview);
    controls->addWidget(new QLabel("Zoom", preview));
    controls->addWidget(Zoom);
    controls->addWidget(IgnoreAlpha);
    controls->addStretch();
    previewLayout->addLayout(controls);
    QScrollArea* scroll = new QScrollArea(preview);
    scroll->setAlignment(Qt::AlignCenter);
    TextureView = new Gfx3DTextureView(scroll);
    scroll->setWidget(TextureView);
    previewLayout->addWidget(scroll, 1);
    PaletteView = new QLabel(preview);
    previewLayout->addWidget(PaletteView);
    TextureDetail = MakeDetail(preview);
    previewLayout->addWidget(TextureDetail);
    TexturesPage->setStretchFactor(0, 2);
    TexturesPage->setStretchFactor(1, 3);
    Tabs->addTab(TexturesPage, "Textures");

    layout->addWidget(Tabs, 1);
    Info = new QLabel("Polygons of the last frame sent to the renderer, in drawing order.", this);
    layout->addWidget(Info);

    connect(LiveUpdate, &QCheckBox::toggled, this, [this](bool on) {
        setLiveRefresh(on);
        if (on)
            requestRefresh();
    });
    connect(reload, &QPushButton::clicked, this, &Gfx3DWindow::requestRefresh);
    connect(Tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (Tabs->widget(index) == TexturesPage)
            requestRefresh();
    });
    connect(PolygonTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        showPolygon(item ? PolygonTree->indexOfTopLevelItem(item) : -1);
    });
    connect(TextureTree, &QTreeWidget::currentItemChanged, this, &Gfx3DWindow::showTexture);
    connect(Zoom, &QSpinBox::valueChanged, this, &Gfx3DWindow::showTexture);
    connect(IgnoreAlpha, &QCheckBox::toggled, this, &Gfx3DWindow::showTexture);
    Wireframe->Picked = [this](int index) { selectPolygon(index); };
    setLiveRefresh(false);
    setInterval(500);
}

void Gfx3DWindow::refresh()
{
    bool withVRAM = Tabs->currentWidget() == TexturesPage;
    fetch([withVRAM](NDS& nds) { return CaptureFrame(nds, withVRAM); },
        [this](const std::shared_ptr<const Gfx3DFrame>& frame) { showFrame(frame); });
}

void Gfx3DWindow::showFrame(const std::shared_ptr<const Gfx3DFrame>& frame)
{
    bool same = Frame && SamePolygons(*Frame, *frame);
    Frame = frame;
    bool newVRAM = !frame->TextureVRAM.empty();
    if (newVRAM)
        VRAM = frame;
    Info->setText(DescribeFrame(*frame));
    Wireframe->setFrame(frame);
    if (!same)
    {
        fillTextures();
        fillPolygons();
    }
    else if (newVRAM)
        showTexture();
}

void Gfx3DWindow::fillPolygons()
{
    int keep = Selected;
    int scroll = PolygonTree->verticalScrollBar()->value();
    PolygonTree->blockSignals(true);
    PolygonTree->clear();
    QList<QTreeWidgetItem*> items;
    for (size_t i = 0; i < Frame->Polygons.size(); i++)
        items.append(new QTreeWidgetItem(PolygonColumns(i, Frame->Polygons[i])));
    PolygonTree->addTopLevelItems(items);
    if (keep >= items.size())
        keep = -1;
    if (keep >= 0)
        PolygonTree->setCurrentItem(items[keep]);
    PolygonTree->blockSignals(false);
    if (!PolygonColumnsSized && !items.isEmpty())
    {
        for (int i = 0; i < PolygonTree->columnCount(); i++)
            PolygonTree->resizeColumnToContents(i);
        PolygonColumnsSized = true;
    }
    PolygonTree->verticalScrollBar()->setValue(scroll);
    showPolygon(keep);
}

void Gfx3DWindow::fillTextures()
{
    QTreeWidgetItem* current = TextureTree->currentItem();
    u64 keep = current ? current->data(0, Qt::UserRole).toULongLong() : 0;
    std::map<u64, int> uses;
    for (const Gfx3DPolygon& poly : Frame->Polygons)
    {
        u64 key = TextureKey(poly);
        if (key)
            uses[key]++;
    }
    TextureTree->blockSignals(true);
    TextureTree->setSortingEnabled(false);
    TextureTree->clear();
    QTreeWidgetItem* restored = nullptr;
    for (auto& [key, count] : uses)
    {
        u32 texParam = key >> 32;
        u32 texPal = key & 0xFFFFFFFF;
        u32 format = TextureFormat(texParam);
        QString name = TextureFormatNames[format];
        if ((texParam & (1u << 29)) && format >= 2 && format <= 4)
            name += " c0";
        QTreeWidgetItem* item = new QTreeWidgetItem(TextureTree, {
            Hex(TextureAddress(texParam), 5),
            name,
            QString("%1x%2").arg(TextureWidth(texParam)).arg(TextureHeight(texParam)),
            format == 7 ? QString("-") : Hex(PaletteAddress(texParam, texPal), 5),
            "",
        });
        item->setData(0, Qt::UserRole, (qulonglong)key);
        item->setData(4, Qt::DisplayRole, count);
        if (key == keep)
            restored = item;
    }
    TextureTree->setSortingEnabled(true);
    TextureTree->setCurrentItem(restored);
    TextureTree->blockSignals(false);
    for (int i = 0; i < TextureTree->columnCount(); i++)
        TextureTree->resizeColumnToContents(i);
    showTexture();
}

void Gfx3DWindow::selectPolygon(int index)
{
    QTreeWidgetItem* item = index >= 0 ? PolygonTree->topLevelItem(index) : nullptr;
    PolygonTree->setCurrentItem(item);
    if (item)
        PolygonTree->scrollToItem(item);
}

void Gfx3DWindow::showPolygon(int index)
{
    Selected = index;
    Wireframe->setSelected(index);
    VertexTree->clear();
    if (!Frame || index < 0 || index >= (int)Frame->Polygons.size())
    {
        PolygonDetail->clear();
        return;
    }
    const Gfx3DPolygon& poly = Frame->Polygons[index];
    for (u32 i = 0; i < poly.NumVertices; i++)
    {
        const Gfx3DVertex& v = poly.Vertices[i];
        QTreeWidgetItem* item = new QTreeWidgetItem(VertexTree, VertexColumns(i, v));
        item->setData(5, Qt::DecorationRole, QColor(Expand5(v.Color[0]), Expand5(v.Color[1]), Expand5(v.Color[2])));
    }
    for (int i = 0; i < VertexTree->columnCount(); i++)
        VertexTree->resizeColumnToContents(i);
    PolygonDetail->setText(DescribePolygon(index, poly));
    u64 key = TextureKey(poly);
    if (!key)
        return;
    for (int i = 0; i < TextureTree->topLevelItemCount(); i++)
    {
        QTreeWidgetItem* item = TextureTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toULongLong() == key)
        {
            TextureTree->setCurrentItem(item);
            break;
        }
    }
}

void Gfx3DWindow::showTexture()
{
    QTreeWidgetItem* item = TextureTree->currentItem();
    if (!item)
    {
        TextureView->setImage(QImage(), 1);
        PaletteView->clear();
        TextureDetail->clear();
        return;
    }
    u64 key = item->data(0, Qt::UserRole).toULongLong();
    u32 texParam = key >> 32;
    u32 texPal = key & 0xFFFFFFFF;
    QString detail = DescribeTexture(texParam, texPal);
    if (!VRAM)
    {
        TextureView->setImage(QImage(), 1);
        PaletteView->clear();
        TextureDetail->setText(detail + "\nTexture VRAM not read yet");
        return;
    }
    QImage image = DecodeTexture(*VRAM, texParam, texPal, IgnoreAlpha->isChecked());
    int longest = std::max(image.width(), image.height());
    int zoom = std::clamp(Zoom->value(), 1, std::max(1, MaxPreviewSize / longest));
    TextureView->setImage(image, zoom);
    PaletteView->setPixmap(PaletteStrip(*VRAM, texParam, texPal));
    TextureDetail->setText(detail);
}
