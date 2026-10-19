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

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Gfx2DWindows.h"
#include "GPU.h"

using namespace melonDS;

namespace
{

struct SourceOption
{
    const char* Label;
    u32 Address;
    u32 Size;
    int DefaultPalette;
};

constexpr int SourceABG = 0;
constexpr int SourceAOBJ = 1;
constexpr int SourceBBG = 2;
constexpr int SourceBOBJ = 3;
constexpr int FirstBankSource = 4;

const SourceOption TileSources[] = {
    {"Engine A BG", 0x06000000, 0x80000, 0},
    {"Engine A OBJ", 0x06400000, 0x40000, 1},
    {"Engine B BG", 0x06200000, 0x20000, 2},
    {"Engine B OBJ", 0x06600000, 0x20000, 3},
    {"Bank A", 0x06800000, 0x20000, -1},
    {"Bank B", 0x06820000, 0x20000, -1},
    {"Bank C", 0x06840000, 0x20000, -1},
    {"Bank D", 0x06860000, 0x20000, -1},
    {"Bank E", 0x06880000, 0x10000, -1},
    {"Bank F", 0x06890000, 0x4000, -1},
    {"Bank G", 0x06894000, 0x4000, -1},
    {"Bank H", 0x06898000, 0x8000, -1},
    {"Bank I", 0x068A0000, 0x4000, -1},
};

struct PaletteOption
{
    const char* Label;
    int Engine;
    bool OBJ;
    bool Extended;
    int Slot;
};

constexpr int FirstExtendedPalette = 4;
constexpr int ExtendedPaletteCount = 10;

const PaletteOption PaletteOptions[] = {
    {"A BG", 0, false, false, 0},
    {"A OBJ", 0, true, false, 0},
    {"B BG", 1, false, false, 0},
    {"B OBJ", 1, true, false, 0},
    {"A BG ext slot 0", 0, false, true, 0},
    {"A BG ext slot 1", 0, false, true, 1},
    {"A BG ext slot 2", 0, false, true, 2},
    {"A BG ext slot 3", 0, false, true, 3},
    {"A OBJ ext", 0, true, true, 0},
    {"B BG ext slot 0", 1, false, true, 0},
    {"B BG ext slot 1", 1, false, true, 1},
    {"B BG ext slot 2", 1, false, true, 2},
    {"B BG ext slot 3", 1, false, true, 3},
    {"B OBJ ext", 1, true, true, 0},
    {"Grayscale", -1, false, false, 0},
};

constexpr int SpriteWidths[4][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {8, 8, 8, 8}};
constexpr int SpriteHeights[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {8, 8, 8, 8}};

constexpr int PaletteBlockSpan = 17;

QRgb ColorToRgb(u16 color)
{
    int r = color & 0x1F;
    int g = (color >> 5) & 0x1F;
    int b = (color >> 10) & 0x1F;
    return qRgb((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
}

QString ColorText(u16 color)
{
    QRgb rgb = ColorToRgb(color);
    return QString("BGR555 %1  RGB %2 %3 %4").arg(Hex(color, 4)).arg(qRed(rgb)).arg(qGreen(rgb)).arg(qBlue(rgb));
}

QString Fixed8(s32 value)
{
    return QString::number(value / 256.0, 'f', 3);
}

QPen OverlayPen(const QColor& color, int width)
{
    QPen pen(color, width);
    pen.setCosmetic(true);
    return pen;
}

u8 ByteAt(const std::vector<u8>& bytes, u32 offset)
{
    return offset < bytes.size() ? bytes[offset] : 0;
}

u16 HalfwordAt(const std::vector<u8>& bytes, u32 offset)
{
    return ByteAt(bytes, offset) | (ByteAt(bytes, offset + 1) << 8);
}

u16 ColorAt(const std::vector<u16>& colors, u32 index)
{
    return index < colors.size() ? colors[index] : 0;
}

QString BankList(u32 mask)
{
    QStringList names;
    for (int i = 0; i < 9; i++)
        if (mask & (1 << i))
            names << QString(QChar('A' + i));
    return names.isEmpty() ? QString("none") : names.join(' ');
}

u64 ReadSource64(const GPU& gpu, int source, u32 offset)
{
    u32 address = TileSources[source].Address + offset;
    switch (source)
    {
    case SourceABG: return gpu.ReadVRAM_ABG<u64>(address);
    case SourceAOBJ: return gpu.ReadVRAM_AOBJ<u64>(address);
    case SourceBBG: return gpu.ReadVRAM_BBG<u64>(address);
    case SourceBOBJ: return gpu.ReadVRAM_BOBJ<u64>(address);
    }
    u64 value;
    memcpy(&value, gpu.VRAM[source - FirstBankSource] + offset, 8);
    return value;
}

std::vector<u8> CopyVRAM(const GPU& gpu, int source, u32 offset, u32 length)
{
    u32 mask = TileSources[source].Size - 1;
    std::vector<u8> bytes((length + 7) & ~7u);
    for (u32 i = 0; i < bytes.size(); i += 8)
    {
        u64 value = ReadSource64(gpu, source, (offset + i) & mask & ~7u);
        memcpy(&bytes[i], &value, 8);
    }
    bytes.resize(length);
    return bytes;
}

u32 MappedBanks(const GPU& gpu, int source)
{
    const u32* map;
    int pages;
    switch (source)
    {
    case SourceABG: map = gpu.VRAMMap_ABG; pages = 0x20; break;
    case SourceAOBJ: map = gpu.VRAMMap_AOBJ; pages = 0x10; break;
    case SourceBBG: map = gpu.VRAMMap_BBG; pages = 8; break;
    case SourceBOBJ: map = gpu.VRAMMap_BOBJ; pages = 8; break;
    default: return 0;
    }
    u32 banks = 0;
    for (int i = 0; i < pages; i++)
        banks |= map[i];
    return banks;
}

u16 ReadBank16(const u8* bank, u32 offset)
{
    u16 value;
    memcpy(&value, bank + offset, 2);
    return value;
}

u16 ReadBGExtPalette(const GPU& gpu, int engine, u32 addr)
{
    addr &= 0x7FFE;
    u16 value = 0;
    if (engine == 0)
    {
        u32 mask = gpu.VRAMMap_ABGExtPal[addr >> 13];
        if (mask & (1 << 4)) value |= ReadBank16(gpu.VRAM_E, addr & 0x7FFF);
        if (mask & (1 << 5)) value |= ReadBank16(gpu.VRAM_F, addr & 0x3FFF);
        if (mask & (1 << 6)) value |= ReadBank16(gpu.VRAM_G, addr & 0x3FFF);
    }
    else if (gpu.VRAMMap_BBGExtPal[addr >> 13] & (1 << 7))
        value |= ReadBank16(gpu.VRAM_H, addr & 0x7FFF);
    return value;
}

u16 ReadOBJExtPalette(const GPU& gpu, int engine, u32 addr)
{
    addr &= 0x1FFE;
    u16 value = 0;
    if (engine == 0)
    {
        if (gpu.VRAMMap_AOBJExtPal & (1 << 5)) value |= ReadBank16(gpu.VRAM_F, addr);
        if (gpu.VRAMMap_AOBJExtPal & (1 << 6)) value |= ReadBank16(gpu.VRAM_G, addr);
    }
    else if (gpu.VRAMMap_BOBJExtPal & (1 << 8))
        value |= ReadBank16(gpu.VRAM_I, addr);
    return value;
}

bool ExtPaletteMapped(const GPU& gpu, int engine, bool obj, int slot)
{
    if (obj)
        return engine ? (gpu.VRAMMap_BOBJExtPal & (1 << 8)) : (gpu.VRAMMap_AOBJExtPal & 0x60);
    return engine ? (gpu.VRAMMap_BBGExtPal[slot] & (1 << 7)) : (gpu.VRAMMap_ABGExtPal[slot] & 0x70);
}

u16 PaletteColor(const GPU& gpu, int engine, bool obj, bool extended, int slot, u32 index)
{
    if (!extended)
        return gpu.ReadPalette<u16>(engine * 0x400 + (obj ? 0x200 : 0) + (index & 0xFF) * 2);
    if (obj)
        return ReadOBJExtPalette(gpu, engine, index * 2);
    return ReadBGExtPalette(gpu, engine, slot * 0x2000 + index * 2);
}

std::vector<u16> ReadColors(const GPU& gpu, int engine, bool obj, bool extended, int slot, u32 first, u32 count)
{
    std::vector<u16> colors(count);
    for (u32 i = 0; i < count; i++)
        colors[i] = PaletteColor(gpu, engine, obj, extended, slot, first + i);
    return colors;
}

struct PaletteBlock
{
    const u16* Colors;
    QString Name;
    bool Mapped;
    bool Extended;
    u32 Address;
};

int PaletteBlocksPerRow(int view)
{
    return view == 0 ? 2 : 4;
}

std::vector<PaletteBlock> PaletteBlocks(const PaletteWindow::Snapshot& data, int view)
{
    std::vector<PaletteBlock> blocks;
    if (view == 0)
    {
        for (int i = 0; i < 4; i++)
            blocks.push_back({&data.Standard[i * 256], PaletteOptions[i].Label, true, false, 0x05000000u + i * 0x200});
        return blocks;
    }
    const PaletteOption& option = PaletteOptions[FirstExtendedPalette + view - 1];
    for (int number = 0; number < 16; number++)
    {
        PaletteBlock block;
        block.Name = QString("%1 palette %2").arg(option.Label).arg(number);
        block.Extended = true;
        if (option.OBJ)
        {
            block.Colors = &data.OBJExtended[option.Engine][number * 256];
            block.Mapped = data.OBJExtendedMapped[option.Engine];
            block.Address = number * 0x200;
        }
        else
        {
            block.Colors = &data.BGExtended[option.Engine][(option.Slot * 16 + number) * 256];
            block.Mapped = data.BGExtendedMapped[option.Engine][option.Slot];
            block.Address = option.Slot * 0x2000 + number * 0x200;
        }
        blocks.push_back(block);
    }
    return blocks;
}

bool PaletteCell(QPoint pixel, int perRow, int count, int& block, int& index)
{
    int column = pixel.x() % PaletteBlockSpan;
    int row = pixel.y() % PaletteBlockSpan;
    if (column == 16 || row == 16)
        return false;
    block = (pixel.y() / PaletteBlockSpan) * perRow + pixel.x() / PaletteBlockSpan;
    index = row * 16 + column;
    return block < count;
}

u32 TileBytes(bool colors256)
{
    return colors256 ? 64 : 32;
}

int TileCount(const TileWindow::Snapshot& data)
{
    return int(data.Bytes.size() / TileBytes(data.Asked.Colors256));
}

int TilePixelIndex(const TileWindow::Snapshot& data, int tile, int x, int y)
{
    if (data.Asked.Colors256)
        return ByteAt(data.Bytes, tile * 64 + y * 8 + x);
    u8 pair = ByteAt(data.Bytes, tile * 32 + y * 4 + x / 2);
    return (x & 1) ? pair >> 4 : pair & 0xF;
}

QRgb TileIndexColor(const TileWindow::Snapshot& data, int index)
{
    const PaletteOption& option = PaletteOptions[data.Asked.Palette];
    if (option.Engine < 0)
    {
        int level = data.Asked.Colors256 ? index : index * 17;
        return qRgb(level, level, level);
    }
    bool rowed = !data.Asked.Colors256 && !option.Extended;
    return ColorToRgb(ColorAt(data.Colors, rowed ? data.Asked.PaletteNumber * 16 + index : index));
}

using MapKind = BGMapWindow::Kind;

MapKind KindForMode(int engine, int bg, u32 dispCnt, u16 bgCnt)
{
    int mode = dispCnt & 7;
    MapKind kind = MapKind::Unavailable;
    switch (bg)
    {
    case 0:
        if (engine == 0 && (dispCnt & (1 << 3)))
            return MapKind::ThreeD;
        kind = mode == 6 ? MapKind::Unavailable : MapKind::Text;
        break;
    case 1:
        kind = mode == 6 ? MapKind::Unavailable : MapKind::Text;
        break;
    case 2:
        if (mode == 0 || mode == 1 || mode == 3)
            kind = MapKind::Text;
        else if (mode == 2 || mode == 4)
            kind = MapKind::Affine;
        else if (mode == 5)
            kind = MapKind::ExtendedTiles;
        else if (mode == 6)
            kind = MapKind::Large;
        break;
    case 3:
        if (mode == 0)
            kind = MapKind::Text;
        else if (mode == 1 || mode == 2)
            kind = MapKind::Affine;
        else if (mode <= 5)
            kind = MapKind::ExtendedTiles;
        break;
    }
    if (kind == MapKind::ExtendedTiles && (bgCnt & (1 << 7)))
        kind = (bgCnt & (1 << 2)) ? MapKind::ExtendedDirect : MapKind::ExtendedBitmap;
    return kind;
}

BGMapWindow::MapLayout DescribeMap(int engine, int bg, u32 dispCnt, u16 bgCnt)
{
    static const int bitmapWidths[4] = {128, 256, 512, 512};
    static const int bitmapHeights[4] = {128, 256, 256, 512};
    static const int largeWidths[4] = {512, 1024, 512, 512};
    static const int largeHeights[4] = {1024, 512, 256, 512};

    BGMapWindow::MapLayout layout;
    layout.Type = KindForMode(engine, bg, dispCnt, bgCnt);
    int sizeBits = bgCnt >> 14;
    u32 charBase = ((bgCnt >> 2) & 0xF) * 0x4000;
    u32 mapBase = ((bgCnt >> 8) & 0x1F) * 0x800;
    if (engine == 0)
    {
        charBase += ((dispCnt >> 24) & 7) * 0x10000;
        mapBase += ((dispCnt >> 27) & 7) * 0x10000;
    }
    bool extPalettes = dispCnt & (1 << 30);
    layout.Wrap = bgCnt & (1 << 13);
    switch (layout.Type)
    {
    case MapKind::Text:
        layout.Width = (sizeBits & 1) ? 512 : 256;
        layout.Height = (sizeBits & 2) ? 512 : 256;
        layout.Colors256 = bgCnt & (1 << 7);
        layout.ExtPalette = layout.Colors256 && extPalettes;
        layout.ExtPaletteSlot = (bg < 2 && (bgCnt & (1 << 13))) ? bg + 2 : bg;
        layout.MapBase = mapBase;
        layout.MapBytes = (layout.Width / 8) * (layout.Height / 8) * 2;
        layout.GraphicsBase = charBase;
        layout.GraphicsBytes = 1024 * TileBytes(layout.Colors256);
        layout.Wrap = true;
        break;
    case MapKind::Affine:
        layout.Width = layout.Height = 128 << sizeBits;
        layout.Colors256 = true;
        layout.MapBase = mapBase;
        layout.MapBytes = (layout.Width / 8) * (layout.Height / 8);
        layout.GraphicsBase = charBase;
        layout.GraphicsBytes = 256 * 64;
        break;
    case MapKind::ExtendedTiles:
        layout.Width = layout.Height = 128 << sizeBits;
        layout.Colors256 = true;
        layout.ExtPalette = extPalettes;
        layout.ExtPaletteSlot = bg;
        layout.MapBase = mapBase;
        layout.MapBytes = (layout.Width / 8) * (layout.Height / 8) * 2;
        layout.GraphicsBase = charBase;
        layout.GraphicsBytes = 1024 * 64;
        break;
    case MapKind::ExtendedBitmap:
    case MapKind::ExtendedDirect:
        layout.Width = bitmapWidths[sizeBits];
        layout.Height = bitmapHeights[sizeBits];
        layout.Colors256 = layout.Type == MapKind::ExtendedBitmap;
        layout.GraphicsBase = ((bgCnt >> 8) & 0x1F) * 0x4000;
        layout.GraphicsBytes = layout.Width * layout.Height * (layout.Colors256 ? 1 : 2);
        break;
    case MapKind::Large:
        layout.Width = largeWidths[sizeBits];
        layout.Height = largeHeights[sizeBits];
        layout.Colors256 = true;
        layout.GraphicsBytes = layout.Width * layout.Height;
        break;
    case MapKind::Unavailable:
    case MapKind::ThreeD:
        break;
    }
    return layout;
}

bool IsTiled(MapKind kind)
{
    return kind == MapKind::Text || kind == MapKind::Affine || kind == MapKind::ExtendedTiles;
}

const char* KindName(MapKind kind)
{
    switch (kind)
    {
    case MapKind::Unavailable: return "not available in this BG mode";
    case MapKind::ThreeD: return "3D";
    case MapKind::Text: return "text";
    case MapKind::Affine: return "affine";
    case MapKind::ExtendedTiles: return "extended affine, rotscale tiles";
    case MapKind::ExtendedBitmap: return "extended affine, 256-color bitmap";
    case MapKind::ExtendedDirect: return "extended affine, direct-color bitmap";
    case MapKind::Large: return "large 256-color bitmap";
    }
    return "";
}

u32 TextEntryOffset(int width, int column, int row)
{
    u32 block = (column >= 32 ? 1 : 0) + (row >= 32 ? (width > 256 ? 2 : 1) : 0);
    return block * 0x800 + ((row & 31) * 32 + (column & 31)) * 2;
}

struct MapSample
{
    bool HasEntry = false;
    u32 EntryOffset = 0;
    u16 Entry = 0;
    u32 PixelOffset = 0;
    u16 Value = 0;
    bool Opaque = false;
    u16 Color = 0;
};

MapSample SampleMap(const BGMapWindow::Snapshot& data, int x, int y)
{
    const BGMapWindow::MapLayout& layout = data.Layout;
    MapSample sample;
    switch (layout.Type)
    {
    case MapKind::Text:
    case MapKind::Affine:
    case MapKind::ExtendedTiles:
        {
            int column = x / 8;
            int row = y / 8;
            int columns = layout.Width / 8;
            u32 entryOffset;
            if (layout.Type == MapKind::Text)
                entryOffset = TextEntryOffset(layout.Width, column, row);
            else if (layout.Type == MapKind::Affine)
                entryOffset = row * columns + column;
            else
                entryOffset = (row * columns + column) * 2;
            u16 entry = layout.Type == MapKind::Affine ? ByteAt(data.MapData, entryOffset) : HalfwordAt(data.MapData, entryOffset);
            sample.HasEntry = true;
            sample.EntryOffset = layout.MapBase + entryOffset;
            sample.Entry = entry;
            int px = x & 7;
            int py = y & 7;
            u32 tile = entry;
            int palette = 0;
            if (layout.Type != MapKind::Affine)
            {
                tile = entry & 0x3FF;
                palette = entry >> 12;
                if (entry & (1 << 10))
                    px = 7 - px;
                if (entry & (1 << 11))
                    py = 7 - py;
            }
            if (layout.Colors256)
            {
                u32 offset = tile * 64 + py * 8 + px;
                sample.PixelOffset = layout.GraphicsBase + offset;
                sample.Value = ByteAt(data.GraphicsData, offset);
                sample.Color = layout.ExtPalette ? ColorAt(data.ExtPalette, palette * 256 + sample.Value) : ColorAt(data.Palette, sample.Value);
            }
            else
            {
                u32 offset = tile * 32 + py * 4 + px / 2;
                u8 pair = ByteAt(data.GraphicsData, offset);
                sample.PixelOffset = layout.GraphicsBase + offset;
                sample.Value = (px & 1) ? pair >> 4 : pair & 0xF;
                sample.Color = ColorAt(data.Palette, palette * 16 + sample.Value);
            }
            sample.Opaque = sample.Value != 0;
        }
        break;
    case MapKind::ExtendedBitmap:
    case MapKind::Large:
        {
            u32 offset = y * layout.Width + x;
            sample.PixelOffset = layout.GraphicsBase + offset;
            sample.Value = ByteAt(data.GraphicsData, offset);
            sample.Color = ColorAt(data.Palette, sample.Value);
            sample.Opaque = sample.Value != 0;
        }
        break;
    case MapKind::ExtendedDirect:
        {
            u32 offset = (y * layout.Width + x) * 2;
            sample.PixelOffset = layout.GraphicsBase + offset;
            sample.Value = HalfwordAt(data.GraphicsData, offset);
            sample.Color = sample.Value & 0x7FFF;
            sample.Opaque = sample.Value & 0x8000;
        }
        break;
    case MapKind::Unavailable:
    case MapKind::ThreeD:
        break;
    }
    return sample;
}

QString DescribeMapParams(const BGMapWindow::Snapshot& data)
{
    static const char* displayModes[4] = {"off", "graphics", "VRAM", "main memory FIFO"};
    const BGMapWindow::MapLayout& layout = data.Layout;
    u32 base = TileSources[data.Engine ? SourceBBG : SourceABG].Address;
    u32 mask = TileSources[data.Engine ? SourceBBG : SourceABG].Size - 1;
    QStringList lines;
    lines << QString("DISPCNT   %1").arg(Hex(data.DispCnt));
    lines << QString("BG mode   %1").arg(data.DispCnt & 7);
    lines << QString("display   %1").arg(displayModes[(data.DispCnt >> 16) & 3]);
    lines << QString("layer     %1").arg((data.DispCnt & (0x100 << data.BG)) ? "enabled" : "disabled");
    lines << QString("BG%1CNT    %2").arg(data.BG).arg(Hex(data.BGCnt, 4));
    lines << QString("type      %1").arg(KindName(layout.Type));
    if (!layout.Width)
        return lines.join('\n');
    lines << QString("size      %1x%2").arg(layout.Width).arg(layout.Height);
    lines << QString("priority  %1").arg(data.BGCnt & 3);
    lines << QString("mosaic    %1 (%2x%3)").arg((data.BGCnt & (1 << 6)) ? "on" : "off").arg(data.MosaicWidth + 1).arg(data.MosaicHeight + 1);
    if (IsTiled(layout.Type))
    {
        lines << QString("colors    %1").arg(layout.Colors256 ? 256 : 16);
        if (layout.ExtPalette)
            lines << QString("palette   ext slot %1%2").arg(layout.ExtPaletteSlot).arg(data.ExtPaletteMapped ? "" : " (not mapped)");
        else
            lines << QString("palette   standard");
        lines << QString("chars     %1").arg(Hex(base + (layout.GraphicsBase & mask)));
        lines << QString("map       %1").arg(Hex(base + (layout.MapBase & mask)));
    }
    else
    {
        lines << QString("colors    %1").arg(layout.Colors256 ? "256, standard palette" : "direct");
        lines << QString("bitmap    %1").arg(Hex(base + (layout.GraphicsBase & mask)));
    }
    if (layout.Type == MapKind::Text)
    {
        lines << QString("scroll    H %1  V %2").arg(data.XOffset & 0x1FF).arg(data.YOffset & 0x1FF);
        return lines.join('\n');
    }
    lines << QString("wrap      %1").arg(layout.Wrap ? "on" : "off");
    lines << QString("PA %1  PB %2").arg(Fixed8(data.RotA)).arg(Fixed8(data.RotB));
    lines << QString("PC %1  PD %2").arg(Fixed8(data.RotC)).arg(Fixed8(data.RotD));
    lines << QString("ref X %1").arg(Fixed8(data.RefX));
    lines << QString("ref Y %1").arg(Fixed8(data.RefY));
    return lines.join('\n');
}

struct Sprite
{
    u16 Attr[3];
    int X;
    int Y;
    int Shape;
    int Size;
    int Width;
    int Height;
    int Mode;
    int Tile;
    int Palette;
    int Priority;
    int AffineGroup;
    bool Affine;
    bool DoubleSize;
    bool Hidden;
    bool Colors256;
    bool Mosaic;
    bool HFlip;
    bool VFlip;
};

Sprite DecodeSprite(const std::vector<u16>& oam, int index)
{
    Sprite sprite;
    for (int i = 0; i < 3; i++)
        sprite.Attr[i] = oam[index * 4 + i];
    u16 attr0 = sprite.Attr[0];
    u16 attr1 = sprite.Attr[1];
    u16 attr2 = sprite.Attr[2];
    sprite.Y = attr0 & 0xFF;
    sprite.Affine = attr0 & (1 << 8);
    sprite.DoubleSize = sprite.Affine && (attr0 & (1 << 9));
    sprite.Hidden = !sprite.Affine && (attr0 & (1 << 9));
    sprite.Mode = (attr0 >> 10) & 3;
    sprite.Mosaic = attr0 & (1 << 12);
    sprite.Colors256 = attr0 & (1 << 13);
    sprite.Shape = attr0 >> 14;
    sprite.X = attr1 & 0x1FF;
    if (sprite.X >= 256)
        sprite.X -= 512;
    sprite.AffineGroup = sprite.Affine ? (attr1 >> 9) & 0x1F : -1;
    sprite.HFlip = !sprite.Affine && (attr1 & (1 << 12));
    sprite.VFlip = !sprite.Affine && (attr1 & (1 << 13));
    sprite.Size = attr1 >> 14;
    sprite.Tile = attr2 & 0x3FF;
    sprite.Priority = (attr2 >> 10) & 3;
    sprite.Palette = attr2 >> 12;
    sprite.Width = SpriteWidths[sprite.Shape][sprite.Size];
    sprite.Height = SpriteHeights[sprite.Shape][sprite.Size];
    return sprite;
}

u32 SpriteDataOffset(u32 dispCnt, const Sprite& sprite)
{
    u32 tile = sprite.Tile;
    if (sprite.Mode == 3)
    {
        if (dispCnt & (1 << 6))
            return tile << (7 + ((dispCnt >> 22) & 1));
        if (dispCnt & (1 << 5))
            return ((tile & 0x1F) << 4) + ((tile & 0x3E0) << 7);
        return ((tile & 0x0F) << 4) + ((tile & 0x3F0) << 7);
    }
    if (dispCnt & (1 << 4))
        return (tile << ((dispCnt >> 20) & 3)) * 32;
    return tile * 32;
}

bool SpriteTexel(const OAMWindow::Snapshot& data, const Sprite& sprite, int x, int y, QRgb& rgb)
{
    if (data.VRAM.empty())
        return false;
    u32 mask = data.VRAM.size() - 1;
    u32 base = SpriteDataOffset(data.DispCnt, sprite);
    if (sprite.Mode == 3)
    {
        bool linear = data.DispCnt & (1 << 6);
        bool wide = data.DispCnt & (1 << 5);
        if (linear && wide)
            return false;
        u32 stride = linear ? sprite.Width * 2 : (wide ? 512 : 256);
        u32 offset = base + y * stride + x * 2;
        u16 color = data.VRAM[offset & mask] | (data.VRAM[(offset + 1) & mask] << 8);
        if (!(color & 0x8000))
            return false;
        rgb = ColorToRgb(color);
        return true;
    }
    u32 tileUnits = sprite.Colors256 ? 2 : 1;
    u32 rowUnits = (data.DispCnt & (1 << 4)) ? (sprite.Width / 8) * tileUnits : 32;
    u32 tile = base + ((y / 8) * rowUnits + (x / 8) * tileUnits) * 32;
    int index;
    if (sprite.Colors256)
        index = data.VRAM[(tile + (y & 7) * 8 + (x & 7)) & mask];
    else
    {
        u8 pair = data.VRAM[(tile + (y & 7) * 4 + (x & 7) / 2) & mask];
        index = (x & 1) ? pair >> 4 : pair & 0xF;
    }
    if (!index)
        return false;
    u16 color;
    if (!sprite.Colors256)
        color = ColorAt(data.Palette, sprite.Palette * 16 + index);
    else if (data.DispCnt & (1u << 31))
        color = ColorAt(data.ExtPalette, sprite.Palette * 256 + index);
    else
        color = ColorAt(data.Palette, index);
    rgb = ColorToRgb(color);
    return true;
}

void SpriteMatrix(const OAMWindow::Snapshot& data, const Sprite& sprite, s16& pa, s16& pb, s16& pc, s16& pd)
{
    int group = sprite.AffineGroup * 16;
    pa = data.OAM[group + 3];
    pb = data.OAM[group + 7];
    pc = data.OAM[group + 11];
    pd = data.OAM[group + 15];
}

QImage RenderSprite(const OAMWindow::Snapshot& data, const Sprite& sprite, bool transform)
{
    bool rotscale = sprite.Affine && transform;
    int scale = (rotscale && sprite.DoubleSize) ? 2 : 1;
    int width = sprite.Width * scale;
    int height = sprite.Height * scale;
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
    if (rotscale)
        SpriteMatrix(data, sprite, pa, pb, pc, pd);
    for (int y = 0; y < height; y++)
    {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; x++)
        {
            int tx = x;
            int ty = y;
            if (rotscale)
            {
                s32 rx = (x - width / 2) * pa + (y - height / 2) * pb + (sprite.Width << 7);
                s32 ry = (x - width / 2) * pc + (y - height / 2) * pd + (sprite.Height << 7);
                if (rx < 0 || ry < 0 || rx >= (sprite.Width << 8) || ry >= (sprite.Height << 8))
                    continue;
                tx = rx >> 8;
                ty = ry >> 8;
            }
            else
            {
                if (sprite.HFlip)
                    tx = sprite.Width - 1 - x;
                if (sprite.VFlip)
                    ty = sprite.Height - 1 - y;
            }
            QRgb rgb;
            if (SpriteTexel(data, sprite, tx, ty, rgb))
                line[x] = rgb;
        }
    }
    return image;
}

QString ObjectMappingText(const OAMWindow::Snapshot& data)
{
    u32 dispCnt = data.DispCnt;
    QString tiles = (dispCnt & (1 << 4)) ? QString("1D, boundary %1 bytes").arg(32 << ((dispCnt >> 20) & 3))
                                         : QString("2D, 32 tiles per row");
    QString bitmaps;
    if (dispCnt & (1 << 6))
        bitmaps = (dispCnt & (1 << 5)) ? QString("1D, reserved") : QString("1D, boundary %1 bytes").arg(128 << ((dispCnt >> 22) & 1));
    else
        bitmaps = (dispCnt & (1 << 5)) ? QString("2D, 256 pixels wide") : QString("2D, 128 pixels wide");
    QString ext = (dispCnt & (1u << 31)) ? (data.ExtPaletteMapped ? "on" : "on, not mapped") : "off";
    return QString("DISPCNT %1   OBJ layer %2   tiles %3   bitmaps %4   ext palettes %5")
        .arg(Hex(dispCnt)).arg((dispCnt & (1 << 12)) ? "on" : "off").arg(tiles).arg(bitmaps).arg(ext);
}

QString SpriteDetails(const OAMWindow::Snapshot& data, const Sprite& sprite, int index)
{
    const SourceOption& source = TileSources[data.Engine ? SourceBOBJ : SourceAOBJ];
    QStringList lines;
    lines << QString("sprite %1  %2x%3  at %4, %5").arg(index).arg(sprite.Width).arg(sprite.Height).arg(sprite.X).arg(sprite.Y);
    lines << QString("attr   %1 %2 %3").arg(Hex(sprite.Attr[0], 4)).arg(Hex(sprite.Attr[1], 4)).arg(Hex(sprite.Attr[2], 4));
    lines << QString("data   %1  tile %2").arg(Hex(source.Address + (SpriteDataOffset(data.DispCnt, sprite) & (source.Size - 1)))).arg(Hex(sprite.Tile, 3));
    if (sprite.Mode == 3)
        lines << QString("colors direct, alpha %1").arg(sprite.Palette);
    else if (!sprite.Colors256)
        lines << QString("colors 4bpp, palette %1").arg(sprite.Palette);
    else if (data.DispCnt & (1u << 31))
        lines << QString("colors 8bpp, ext palette %1%2").arg(sprite.Palette).arg(data.ExtPaletteMapped ? "" : " (not mapped)");
    else
        lines << QString("colors 8bpp, standard palette");
    if (sprite.Affine)
    {
        s16 pa, pb, pc, pd;
        SpriteMatrix(data, sprite, pa, pb, pc, pd);
        lines << QString("affine group %1%2").arg(sprite.AffineGroup).arg(sprite.DoubleSize ? ", double size" : "");
        lines << QString("PA %1  PB %2  PC %3  PD %4").arg(Fixed8(pa)).arg(Fixed8(pb)).arg(Fixed8(pc)).arg(Fixed8(pd));
    }
    return lines.join('\n');
}

QScrollArea* ScrollFor(QWidget* parent, Gfx2DImageView* view)
{
    QScrollArea* scroll = new QScrollArea(parent);
    scroll->setWidget(view);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return scroll;
}

QLabel* MonoLabel(QWidget* parent)
{
    QLabel* label = new QLabel(parent);
    label->setFont(MonoFont());
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

}

Gfx2DImageView::Gfx2DImageView(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    Checker = QPixmap(16, 16);
    Checker.fill(QColor(0xC8, 0xC8, 0xC8));
    QPainter painter(&Checker);
    painter.fillRect(0, 0, 8, 8, QColor(0x98, 0x98, 0x98));
    painter.fillRect(8, 8, 8, 8, QColor(0x98, 0x98, 0x98));
    updateSize();
}

void Gfx2DImageView::setImage(const QImage& image)
{
    Image = image;
    updateSize();
    update();
}

void Gfx2DImageView::setZoom(int zoom)
{
    Zoom = std::max(1, zoom);
    updateSize();
    update();
}

void Gfx2DImageView::updateSize()
{
    QSize size = Image.isNull() ? QSize(1, 1) : Image.size() * Zoom;
    if (minimumSize() != size || maximumSize() != size)
        setFixedSize(size);
}

bool Gfx2DImageView::pixelAt(const QPointF& pos, QPoint& pixel) const
{
    if (Image.isNull() || pos.x() < 0 || pos.y() < 0)
        return false;
    pixel = QPoint(int(pos.x()) / Zoom, int(pos.y()) / Zoom);
    return Image.rect().contains(pixel);
}

void Gfx2DImageView::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    QRect exposed = event->rect();
    painter.fillRect(exposed, QBrush(Checker));
    if (Image.isNull())
        return;
    QRect source(exposed.left() / Zoom, exposed.top() / Zoom, exposed.width() / Zoom + 2, exposed.height() / Zoom + 2);
    source &= Image.rect();
    painter.drawImage(QRect(source.topLeft() * Zoom, source.size() * Zoom), Image, source);
    if (Overlay)
    {
        painter.scale(Zoom, Zoom);
        painter.setBrush(Qt::NoBrush);
        Overlay(painter);
    }
}

void Gfx2DImageView::mouseMoveEvent(QMouseEvent* event)
{
    QPoint pixel;
    if (Hovered && pixelAt(event->position(), pixel))
        Hovered(pixel);
}

void Gfx2DImageView::mousePressEvent(QMouseEvent* event)
{
    QPoint pixel;
    if (event->button() == Qt::LeftButton && Clicked && pixelAt(event->position(), pixel))
        Clicked(pixel);
}

PaletteWindow::PaletteWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Palettes", parent)
{
    resize(560, 620);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    View = new QComboBox(this);
    View->addItem("Standard palettes");
    for (int i = 0; i < ExtendedPaletteCount; i++)
        View->addItem(PaletteOptions[FirstExtendedPalette + i].Label);
    Swatch = new QSpinBox(this);
    Swatch->setRange(4, 32);
    Swatch->setValue(12);
    Swatch->setSuffix(" px");
    row->addWidget(new QLabel("Show", this));
    row->addWidget(View);
    row->addWidget(new QLabel("Swatch", this));
    row->addWidget(Swatch);
    row->addStretch();
    layout->addLayout(row);
    Legend = new QLabel(this);
    layout->addWidget(Legend);
    Image = new Gfx2DImageView(this);
    Image->setZoom(Swatch->value());
    layout->addWidget(ScrollFor(this, Image), 1);
    Hover = MonoLabel(this);
    Selection = MonoLabel(this);
    layout->addWidget(Hover);
    layout->addWidget(Selection);

    Image->Hovered = [this](QPoint pixel) { Hover->setText(describe(pixel)); };
    Image->Clicked = [this](QPoint pixel) {
        QString text = describe(pixel);
        if (text.isEmpty())
            return;
        Selected = pixel;
        Selection->setText("Selected: " + text);
        Image->update();
    };
    Image->Overlay = [this](QPainter& painter) {
        if (Selected.x() < 0)
            return;
        painter.setPen(OverlayPen(Qt::white, 2));
        painter.drawRect(QRectF(Selected.x(), Selected.y(), 1, 1));
    };
    connect(View, &QComboBox::currentIndexChanged, this, [this] {
        Selected = QPoint(-1, -1);
        Selection->clear();
        render();
    });
    connect(Swatch, &QSpinBox::valueChanged, this, [this](int size) { Image->setZoom(size); });
}

void PaletteWindow::refresh()
{
    fetch([](NDS& nds) {
        const GPU& gpu = nds.GPU;
        Snapshot s;
        s.Standard.resize(1024);
        for (u32 i = 0; i < 1024; i++)
            s.Standard[i] = gpu.ReadPalette<u16>(i * 2);
        for (int engine = 0; engine < 2; engine++)
        {
            s.BGExtended[engine] = ReadColors(gpu, engine, false, true, 0, 0, 4 * 16 * 256);
            s.OBJExtended[engine] = ReadColors(gpu, engine, true, true, 0, 0, 16 * 256);
            for (int slot = 0; slot < 4; slot++)
                s.BGExtendedMapped[engine][slot] = ExtPaletteMapped(gpu, engine, false, slot);
            s.OBJExtendedMapped[engine] = ExtPaletteMapped(gpu, engine, true, 0);
        }
        return s;
    }, [this](Snapshot& s) {
        Data = std::move(s);
        HaveData = true;
        render();
    });
}

void PaletteWindow::render()
{
    if (!HaveData)
        return;
    int view = View->currentIndex();
    std::vector<PaletteBlock> blocks = PaletteBlocks(Data, view);
    int perRow = PaletteBlocksPerRow(view);
    int rows = (int(blocks.size()) + perRow - 1) / perRow;
    QImage image(perRow * PaletteBlockSpan - 1, rows * PaletteBlockSpan - 1, QImage::Format_ARGB32);
    image.fill(QColor(0x40, 0x40, 0x40));
    for (int b = 0; b < int(blocks.size()); b++)
    {
        int left = (b % perRow) * PaletteBlockSpan;
        int top = (b / perRow) * PaletteBlockSpan;
        for (int i = 0; i < 256; i++)
            image.setPixel(left + i % 16, top + i / 16, blocks[b].Mapped ? ColorToRgb(blocks[b].Colors[i]) : qRgba(0, 0, 0, 0));
    }
    if (view == 0)
        Legend->setText("Top: engine A BG, OBJ. Bottom: engine B BG, OBJ. 16 colors per row.");
    else
        Legend->setText(QString("%1: palettes 0-15, left to right, top to bottom%2")
            .arg(PaletteOptions[FirstExtendedPalette + view - 1].Label).arg(blocks[0].Mapped ? "" : " (not mapped)"));
    Image->setImage(image);
    if (Selected.x() >= 0)
        Selection->setText("Selected: " + describe(Selected));
}

QString PaletteWindow::describe(QPoint pixel) const
{
    if (!HaveData)
        return {};
    int view = View->currentIndex();
    std::vector<PaletteBlock> blocks = PaletteBlocks(Data, view);
    int block, index;
    if (!PaletteCell(pixel, PaletteBlocksPerRow(view), int(blocks.size()), block, index))
        return {};
    const PaletteBlock& b = blocks[block];
    u32 address = b.Address + index * 2;
    QString where = b.Extended ? QString("ext offset %1").arg(Hex(address, 4)) : Hex(address);
    QString text = QString("%1 index %2 (%3)  %4").arg(b.Name).arg(index).arg(where).arg(ColorText(b.Colors[index]));
    if (!b.Mapped)
        text += "  not mapped";
    return text;
}

bool TileWindow::Request::operator==(const Request& other) const
{
    return Source == other.Source && Colors256 == other.Colors256 && Offset == other.Offset && Length == other.Length
        && Palette == other.Palette && PaletteNumber == other.PaletteNumber;
}

TileWindow::TileWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Tiles", parent)
{
    resize(640, 760);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* top = new QHBoxLayout();
    Source = new QComboBox(this);
    for (const SourceOption& option : TileSources)
        Source->addItem(option.Label);
    Depth = new QComboBox(this);
    Depth->addItems({"4bpp", "8bpp"});
    Palette = new QComboBox(this);
    for (const PaletteOption& option : PaletteOptions)
        Palette->addItem(option.Label);
    PaletteNumber = new QSpinBox(this);
    PaletteNumber->setRange(0, 15);
    PaletteNumber->setPrefix("#");
    top->addWidget(new QLabel("Source", this));
    top->addWidget(Source);
    top->addWidget(Depth);
    top->addWidget(new QLabel("Palette", this));
    top->addWidget(Palette);
    top->addWidget(PaletteNumber);
    top->addStretch();
    layout->addLayout(top);

    QHBoxLayout* second = new QHBoxLayout();
    Offset = new QSpinBox(this);
    Offset->setDisplayIntegerBase(16);
    Offset->setPrefix("0x");
    Offset->setSingleStep(0x20);
    Offset->setRange(0, TileSources[0].Size - 0x20);
    Rows = new QSpinBox(this);
    Rows->setRange(1, 256);
    Rows->setValue(32);
    Zoom = new QSpinBox(this);
    Zoom->setRange(1, 8);
    Zoom->setValue(2);
    Grid = new QCheckBox("Grid", this);
    Grid->setChecked(true);
    second->addWidget(new QLabel("Offset", this));
    second->addWidget(Offset);
    second->addWidget(new QLabel("Tile rows", this));
    second->addWidget(Rows);
    second->addWidget(new QLabel("Zoom", this));
    second->addWidget(Zoom);
    second->addWidget(Grid);
    second->addStretch();
    layout->addLayout(second);

    Mapping = new QLabel(this);
    layout->addWidget(Mapping);
    Image = new Gfx2DImageView(this);
    Image->setZoom(Zoom->value());
    layout->addWidget(ScrollFor(this, Image), 1);
    Hover = MonoLabel(this);
    Selection = MonoLabel(this);
    layout->addWidget(Hover);
    layout->addWidget(Selection);

    Image->Hovered = [this](QPoint pixel) { Hover->setText(describe(pixel, true)); };
    Image->Clicked = [this](QPoint pixel) {
        int tile = (pixel.y() / 8) * 32 + pixel.x() / 8;
        if (!HaveData || tile >= TileCount(Data))
            return;
        SelectedTile = tile;
        Selection->setText("Selected: " + describe(pixel, false));
        Image->update();
    };
    Image->Overlay = [this](QPainter& painter) {
        QSize size = Image->imageSize();
        if (Grid->isChecked())
        {
            painter.setPen(OverlayPen(QColor(0, 0, 0, 110), 1));
            for (int x = 8; x < size.width(); x += 8)
                painter.drawLine(QLineF(x, 0, x, size.height()));
            for (int y = 8; y < size.height(); y += 8)
                painter.drawLine(QLineF(0, y, size.width(), y));
        }
        if (SelectedTile >= 0)
        {
            painter.setPen(OverlayPen(Qt::yellow, 2));
            painter.drawRect(QRectF((SelectedTile % 32) * 8, (SelectedTile / 32) * 8, 8, 8));
        }
    };

    connect(Source, &QComboBox::currentIndexChanged, this, &TileWindow::sourceChanged);
    connect(Depth, &QComboBox::currentIndexChanged, this, [this](int index) {
        Offset->setSingleStep(TileBytes(index == 1));
        SelectedTile = -1;
        Selection->clear();
        requestRefresh();
    });
    connect(Palette, &QComboBox::currentIndexChanged, this, &TileWindow::requestRefresh);
    connect(PaletteNumber, &QSpinBox::valueChanged, this, &TileWindow::requestRefresh);
    connect(Offset, &QSpinBox::valueChanged, this, &TileWindow::requestRefresh);
    connect(Rows, &QSpinBox::valueChanged, this, &TileWindow::requestRefresh);
    connect(Zoom, &QSpinBox::valueChanged, this, [this](int zoom) { Image->setZoom(zoom); });
    connect(Grid, &QCheckBox::toggled, this, [this] { Image->update(); });
}

TileWindow::Request TileWindow::currentRequest() const
{
    Request request;
    request.Source = Source->currentIndex();
    request.Colors256 = Depth->currentIndex() == 1;
    u32 tileBytes = TileBytes(request.Colors256);
    const SourceOption& source = TileSources[request.Source];
    request.Offset = std::min<u32>(Offset->value() & ~0x1Fu, source.Size - tileBytes);
    request.Length = std::min<u32>(Rows->value() * 32 * tileBytes, source.Size - request.Offset);
    request.Palette = Palette->currentIndex();
    request.PaletteNumber = PaletteNumber->value();
    return request;
}

void TileWindow::sourceChanged()
{
    const SourceOption& source = TileSources[Source->currentIndex()];
    Offset->setMaximum(source.Size - 0x20);
    if (source.DefaultPalette >= 0)
        Palette->setCurrentIndex(source.DefaultPalette);
    SelectedTile = -1;
    Selection->clear();
    requestRefresh();
}

void TileWindow::refresh()
{
    Request request = currentRequest();
    fetch([request](NDS& nds) {
        const GPU& gpu = nds.GPU;
        Snapshot s;
        s.Asked = request;
        s.Bytes = CopyVRAM(gpu, request.Source, request.Offset, request.Length);
        const PaletteOption& option = PaletteOptions[request.Palette];
        if (option.Engine >= 0)
        {
            s.Colors = ReadColors(gpu, option.Engine, option.OBJ, option.Extended, option.Slot, option.Extended ? request.PaletteNumber * 256 : 0, 256);
            s.PaletteMapped = !option.Extended || ExtPaletteMapped(gpu, option.Engine, option.OBJ, option.Slot);
        }
        s.MappedBanks = MappedBanks(gpu, request.Source);
        memcpy(s.BankControl, gpu.VRAMCNT, sizeof(s.BankControl));
        return s;
    }, [this](Snapshot& s) {
        Data = std::move(s);
        HaveData = true;
        if (!(Data.Asked == currentRequest()))
            requestRefresh();
        render();
    });
}

void TileWindow::render()
{
    if (!HaveData)
        return;
    int tiles = TileCount(Data);
    int rows = std::max(1, (tiles + 31) / 32);
    QImage image(256, rows * 8, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int tile = 0; tile < tiles; tile++)
    {
        for (int y = 0; y < 8; y++)
        {
            QRgb* line = reinterpret_cast<QRgb*>(image.scanLine((tile / 32) * 8 + y)) + (tile % 32) * 8;
            for (int x = 0; x < 8; x++)
                line[x] = TileIndexColor(Data, TilePixelIndex(Data, tile, x, y));
        }
    }
    Image->setImage(image);

    const SourceOption& source = TileSources[Data.Asked.Source];
    QString text;
    if (Data.Asked.Source < FirstBankSource)
        text = QString("%1 at %2, %3 KB, banks mapped: %4").arg(source.Label).arg(Hex(source.Address)).arg(source.Size / 1024).arg(BankList(Data.MappedBanks));
    else
    {
        u8 control = Data.BankControl[Data.Asked.Source - FirstBankSource];
        text = QString("%1 raw, %2 KB, LCDC address %3, VRAMCNT %4: %5, MST %6, offset %7")
            .arg(source.Label).arg(source.Size / 1024).arg(Hex(source.Address)).arg(Hex(control, 2))
            .arg((control & 0x80) ? "enabled" : "disabled").arg(control & 7).arg((control >> 3) & 3);
    }
    if (!Data.PaletteMapped)
        text += ", palette not mapped";
    Mapping->setText(text);

    if (SelectedTile >= tiles)
    {
        SelectedTile = -1;
        Selection->clear();
    }
    else if (SelectedTile >= 0)
        Selection->setText("Selected: " + describe(QPoint((SelectedTile % 32) * 8, (SelectedTile / 32) * 8), false));
}

QString TileWindow::describe(QPoint pixel, bool withPixel) const
{
    if (!HaveData)
        return {};
    int tile = (pixel.y() / 8) * 32 + pixel.x() / 8;
    if (tile >= TileCount(Data))
        return {};
    u32 tileBytes = TileBytes(Data.Asked.Colors256);
    u32 offset = Data.Asked.Offset + tile * tileBytes;
    u32 number = offset / tileBytes;
    QString text = QString("tile %1 (%2)  address %3  offset %4")
        .arg(Hex(number, 4)).arg(number).arg(Hex(TileSources[Data.Asked.Source].Address + offset)).arg(Hex(offset, 5));
    if (withPixel)
    {
        int x = pixel.x() & 7;
        int y = pixel.y() & 7;
        text += QString("  pixel %1,%2 index %3").arg(x).arg(y).arg(TilePixelIndex(Data, tile, x, y));
    }
    return text;
}

BGMapWindow::BGMapWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "BG map", parent)
{
    resize(960, 680);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    Engine = new QComboBox(this);
    Engine->addItems({"Engine A", "Engine B"});
    BG = new QComboBox(this);
    BG->addItems({"BG0", "BG1", "BG2", "BG3"});
    Zoom = new QSpinBox(this);
    Zoom->setRange(1, 4);
    Zoom->setValue(1);
    Viewport = new QCheckBox("Show viewport", this);
    Viewport->setChecked(true);
    row->addWidget(Engine);
    row->addWidget(BG);
    row->addWidget(new QLabel("Zoom", this));
    row->addWidget(Zoom);
    row->addWidget(Viewport);
    row->addStretch();
    layout->addLayout(row);

    QHBoxLayout* body = new QHBoxLayout();
    Image = new Gfx2DImageView(this);
    body->addWidget(ScrollFor(this, Image), 1);
    Params = MonoLabel(this);
    Params->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    Params->setMinimumWidth(260);
    body->addWidget(Params);
    layout->addLayout(body, 1);
    Hover = MonoLabel(this);
    layout->addWidget(Hover);

    Image->Hovered = [this](QPoint pixel) { Hover->setText(describe(pixel)); };
    Image->Overlay = [this](QPainter& painter) {
        if (HaveData && Viewport->isChecked())
            drawViewport(painter);
    };
    connect(Engine, &QComboBox::currentIndexChanged, this, &BGMapWindow::requestRefresh);
    connect(BG, &QComboBox::currentIndexChanged, this, &BGMapWindow::requestRefresh);
    connect(Zoom, &QSpinBox::valueChanged, this, [this](int zoom) { Image->setZoom(zoom); });
    connect(Viewport, &QCheckBox::toggled, this, [this] { Image->update(); });
}

void BGMapWindow::refresh()
{
    int engine = Engine->currentIndex();
    int bg = BG->currentIndex();
    fetch([engine, bg](NDS& nds) {
        const GPU& gpu = nds.GPU;
        const GPU2D& unit = engine ? gpu.GPU2D_B : gpu.GPU2D_A;
        Snapshot s;
        s.Engine = engine;
        s.BG = bg;
        s.DispCnt = unit.DispCnt;
        s.BGCnt = unit.BGCnt[bg];
        s.XOffset = unit.BGXPos[bg];
        s.YOffset = unit.BGYPos[bg];
        if (bg >= 2)
        {
            s.RotA = unit.BGRotA[bg - 2];
            s.RotB = unit.BGRotB[bg - 2];
            s.RotC = unit.BGRotC[bg - 2];
            s.RotD = unit.BGRotD[bg - 2];
            s.RefX = unit.BGXRefReload[bg - 2];
            s.RefY = unit.BGYRefReload[bg - 2];
        }
        s.MosaicWidth = unit.BGMosaicSize[0];
        s.MosaicHeight = unit.BGMosaicSize[1];
        s.Layout = DescribeMap(engine, bg, s.DispCnt, s.BGCnt);
        int source = engine ? SourceBBG : SourceABG;
        s.MapData = CopyVRAM(gpu, source, s.Layout.MapBase, s.Layout.MapBytes);
        s.GraphicsData = CopyVRAM(gpu, source, s.Layout.GraphicsBase, s.Layout.GraphicsBytes);
        s.Palette = ReadColors(gpu, engine, false, false, 0, 0, 256);
        if (s.Layout.ExtPalette)
        {
            s.ExtPalette = ReadColors(gpu, engine, false, true, s.Layout.ExtPaletteSlot, 0, 16 * 256);
            s.ExtPaletteMapped = ExtPaletteMapped(gpu, engine, false, s.Layout.ExtPaletteSlot);
        }
        return s;
    }, [this](Snapshot& s) {
        Data = std::move(s);
        HaveData = true;
        if (Data.Engine != Engine->currentIndex() || Data.BG != BG->currentIndex())
            requestRefresh();
        render();
    });
}

void BGMapWindow::render()
{
    if (!HaveData)
        return;
    Params->setText(DescribeMapParams(Data));
    const MapLayout& layout = Data.Layout;
    if (!layout.Width)
    {
        Image->setImage(QImage());
        return;
    }
    QImage image(layout.Width, layout.Height, QImage::Format_ARGB32);
    for (int y = 0; y < layout.Height; y++)
    {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < layout.Width; x++)
        {
            MapSample sample = SampleMap(Data, x, y);
            line[x] = sample.Opaque ? ColorToRgb(sample.Color) : qRgba(0, 0, 0, 0);
        }
    }
    Image->setImage(image);
}

void BGMapWindow::drawViewport(QPainter& painter) const
{
    const MapLayout& layout = Data.Layout;
    if (!layout.Width)
        return;
    painter.setPen(OverlayPen(QColor(255, 220, 0), 2));
    if (layout.Type == Kind::Text)
    {
        int x = Data.XOffset & (layout.Width - 1);
        int y = Data.YOffset & (layout.Height - 1);
        for (int dx : {0, -layout.Width})
            for (int dy : {0, -layout.Height})
                painter.drawRect(QRectF(x + dx, y + dy, 256, 192));
        return;
    }
    QPolygonF corners;
    for (QPoint screen : {QPoint(0, 0), QPoint(256, 0), QPoint(256, 192), QPoint(0, 192)})
    {
        corners << QPointF((Data.RefX + screen.x() * Data.RotA + screen.y() * Data.RotB) / 256.0,
                           (Data.RefY + screen.x() * Data.RotC + screen.y() * Data.RotD) / 256.0);
    }
    if (!layout.Wrap)
    {
        painter.drawPolygon(corners);
        return;
    }
    corners.translate(-std::floor(corners[0].x() / layout.Width) * layout.Width, -std::floor(corners[0].y() / layout.Height) * layout.Height);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            painter.drawPolygon(corners.translated(dx * layout.Width, dy * layout.Height));
}

QString BGMapWindow::describe(QPoint pixel) const
{
    if (!HaveData || !Data.Layout.Width)
        return {};
    const SourceOption& source = TileSources[Data.Engine ? SourceBBG : SourceABG];
    u32 mask = source.Size - 1;
    MapSample sample = SampleMap(Data, pixel.x(), pixel.y());
    QString text = QString("x %1 y %2").arg(pixel.x()).arg(pixel.y());
    if (sample.HasEntry)
    {
        bool affine = Data.Layout.Type == Kind::Affine;
        text += QString("  entry %1 = %2").arg(Hex(source.Address + (sample.EntryOffset & mask))).arg(Hex(sample.Entry, affine ? 2 : 4));
        if (affine)
            text += QString("  tile %1").arg(Hex(sample.Entry, 2));
        else
            text += QString("  tile %1 pal %2%3%4").arg(Hex(sample.Entry & 0x3FF, 3)).arg(sample.Entry >> 12)
                .arg((sample.Entry & (1 << 10)) ? " hflip" : "").arg((sample.Entry & (1 << 11)) ? " vflip" : "");
    }
    bool direct = Data.Layout.Type == Kind::ExtendedDirect;
    text += QString("  pixel %1 %2").arg(Hex(source.Address + (sample.PixelOffset & mask)))
        .arg(direct ? Hex(sample.Value, 4) : QString("index %1").arg(sample.Value));
    text += sample.Opaque ? "  " + ColorText(sample.Color) : QString("  transparent");
    return text;
}

OAMWindow::OAMWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "OAM", parent)
{
    resize(1100, 640);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    Engine = new QComboBox(this);
    Engine->addItems({"Engine A", "Engine B"});
    Zoom = new QSpinBox(this);
    Zoom->setRange(1, 8);
    Zoom->setValue(4);
    Transform = new QCheckBox("Apply affine transform", this);
    Transform->setChecked(true);
    row->addWidget(Engine);
    row->addWidget(new QLabel("Preview zoom", this));
    row->addWidget(Zoom);
    row->addWidget(Transform);
    row->addStretch();
    layout->addLayout(row);
    Mapping = MonoLabel(this);
    layout->addWidget(Mapping);

    QHBoxLayout* body = new QHBoxLayout();
    Table = new QTableWidget(128, 12, this);
    Table->setHorizontalHeaderLabels({"#", "X", "Y", "Size", "Shape", "Tile", "Palette", "Prio", "Mode", "Flags", "Affine", "Hidden"});
    Table->setFont(MonoFont());
    Table->verticalHeader()->hide();
    Table->horizontalHeader()->setStretchLastSection(true);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    Table->setSelectionMode(QAbstractItemView::SingleSelection);
    Table->verticalHeader()->setDefaultSectionSize(Table->fontMetrics().height() + 2);
    for (int r = 0; r < 128; r++)
        for (int c = 0; c < 12; c++)
            Table->setItem(r, c, new QTableWidgetItem());
    body->addWidget(Table, 1);
    QVBoxLayout* side = new QVBoxLayout();
    Preview = new Gfx2DImageView(this);
    Preview->setZoom(Zoom->value());
    QScrollArea* scroll = ScrollFor(this, Preview);
    scroll->setMinimumSize(300, 300);
    side->addWidget(scroll, 1);
    Details = MonoLabel(this);
    Details->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    side->addWidget(Details);
    body->addLayout(side);
    layout->addLayout(body, 1);

    connect(Engine, &QComboBox::currentIndexChanged, this, [this] {
        ColumnsSized = false;
        requestRefresh();
    });
    connect(Zoom, &QSpinBox::valueChanged, this, [this](int zoom) { Preview->setZoom(zoom); });
    connect(Transform, &QCheckBox::toggled, this, &OAMWindow::renderPreview);
    connect(Table, &QTableWidget::currentCellChanged, this, &OAMWindow::renderPreview);
}

void OAMWindow::refresh()
{
    int engine = Engine->currentIndex();
    fetch([engine](NDS& nds) {
        const GPU& gpu = nds.GPU;
        Snapshot s;
        s.Engine = engine;
        s.DispCnt = (engine ? gpu.GPU2D_B : gpu.GPU2D_A).DispCnt;
        s.OAM.resize(512);
        for (u32 i = 0; i < 512; i++)
            s.OAM[i] = gpu.ReadOAM<u16>(engine * 0x400 + i * 2);
        s.Palette = ReadColors(gpu, engine, true, false, 0, 0, 256);
        s.ExtPalette = ReadColors(gpu, engine, true, true, 0, 0, 16 * 256);
        s.ExtPaletteMapped = ExtPaletteMapped(gpu, engine, true, 0);
        int source = engine ? SourceBOBJ : SourceAOBJ;
        s.VRAM = CopyVRAM(gpu, source, 0, TileSources[source].Size);
        return s;
    }, [this](Snapshot& s) {
        Data = std::move(s);
        HaveData = true;
        if (Data.Engine != Engine->currentIndex())
            requestRefresh();
        fillTable();
        renderPreview();
    });
}

void OAMWindow::fillTable()
{
    static const char* shapes[4] = {"square", "wide", "tall", "prohibited"};
    static const char* modes[4] = {"normal", "semi-transparent", "window", "bitmap"};
    bool extended = Data.DispCnt & (1u << 31);
    for (int i = 0; i < 128; i++)
    {
        Sprite sprite = DecodeSprite(Data.OAM, i);
        QString paletteText;
        if (sprite.Mode == 3)
            paletteText = QString("alpha %1").arg(sprite.Palette);
        else if (!sprite.Colors256)
            paletteText = QString::number(sprite.Palette);
        else if (extended)
            paletteText = QString("ext %1").arg(sprite.Palette);
        else
            paletteText = "-";
        QStringList flags;
        flags << (sprite.Mode == 3 ? "direct" : sprite.Colors256 ? "8bpp" : "4bpp");
        if (sprite.Mosaic)
            flags << "mosaic";
        if (sprite.HFlip)
            flags << "hflip";
        if (sprite.VFlip)
            flags << "vflip";
        if (sprite.DoubleSize)
            flags << "double";
        QStringList cells = {
            QString::number(i),
            QString::number(sprite.X),
            QString::number(sprite.Y),
            QString("%1x%2").arg(sprite.Width).arg(sprite.Height),
            shapes[sprite.Shape],
            Hex(sprite.Tile, 3),
            paletteText,
            QString::number(sprite.Priority),
            modes[sprite.Mode],
            flags.join(' '),
            sprite.Affine ? QString::number(sprite.AffineGroup) : QString("-"),
            sprite.Hidden ? "yes" : "",
        };
        QVariant foreground = sprite.Hidden ? QVariant(QBrush(Qt::gray)) : QVariant();
        for (int c = 0; c < int(cells.size()); c++)
        {
            QTableWidgetItem* item = Table->item(i, c);
            if (item->text() != cells[c])
                item->setText(cells[c]);
            if (item->data(Qt::ForegroundRole) != foreground)
                item->setData(Qt::ForegroundRole, foreground);
        }
    }
    if (!ColumnsSized)
    {
        Table->resizeColumnsToContents();
        ColumnsSized = true;
    }
    Mapping->setText(ObjectMappingText(Data));
}

void OAMWindow::renderPreview()
{
    int row = Table->currentRow();
    if (!HaveData || row < 0 || row >= 128)
    {
        Preview->setImage(QImage());
        Details->setText(HaveData ? "Select a sprite." : "");
        return;
    }
    Sprite sprite = DecodeSprite(Data.OAM, row);
    Preview->setImage(RenderSprite(Data, sprite, Transform->isChecked()));
    Details->setText(SpriteDetails(Data, sprite, row));
}
