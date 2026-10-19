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

#ifndef GFX2DWINDOWS_H
#define GFX2DWINDOWS_H

#include <functional>
#include <vector>

#include <QImage>
#include <QPixmap>

#include "DebugTool.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPainter;
class QSpinBox;
class QTableWidget;

class Gfx2DImageView : public QWidget
{
public:
    explicit Gfx2DImageView(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setZoom(int zoom);
    QSize imageSize() const { return Image.size(); }

    std::function<void(QPoint)> Hovered;
    std::function<void(QPoint)> Clicked;
    std::function<void(QPainter&)> Overlay;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QImage Image;
    QPixmap Checker;
    int Zoom = 1;

    void updateSize();
    bool pixelAt(const QPointF& pos, QPoint& pixel) const;
};

class PaletteWindow : public DebugTool
{
    Q_OBJECT

public:
    PaletteWindow(DebugBridge* bridge, QWidget* parent = nullptr);

    struct Snapshot
    {
        std::vector<melonDS::u16> Standard;
        std::vector<melonDS::u16> BGExtended[2];
        std::vector<melonDS::u16> OBJExtended[2];
        bool BGExtendedMapped[2][4] {};
        bool OBJExtendedMapped[2] {};
    };

protected:
    void refresh() override;

private:
    QComboBox* View;
    QSpinBox* Swatch;
    QLabel* Legend;
    Gfx2DImageView* Image;
    QLabel* Hover;
    QLabel* Selection;
    Snapshot Data;
    bool HaveData = false;
    QPoint Selected {-1, -1};

    void render();
    QString describe(QPoint pixel) const;
};

class TileWindow : public DebugTool
{
    Q_OBJECT

public:
    TileWindow(DebugBridge* bridge, QWidget* parent = nullptr);

    struct Request
    {
        int Source = 0;
        bool Colors256 = false;
        melonDS::u32 Offset = 0;
        melonDS::u32 Length = 0;
        int Palette = 0;
        int PaletteNumber = 0;

        bool operator==(const Request& other) const;
    };

    struct Snapshot
    {
        Request Asked;
        std::vector<melonDS::u8> Bytes;
        std::vector<melonDS::u16> Colors;
        bool PaletteMapped = true;
        melonDS::u32 MappedBanks = 0;
        melonDS::u8 BankControl[9] {};
    };

protected:
    void refresh() override;

private:
    QComboBox* Source;
    QComboBox* Depth;
    QComboBox* Palette;
    QSpinBox* PaletteNumber;
    QSpinBox* Offset;
    QSpinBox* Rows;
    QSpinBox* Zoom;
    QCheckBox* Grid;
    QLabel* Mapping;
    Gfx2DImageView* Image;
    QLabel* Hover;
    QLabel* Selection;
    Snapshot Data;
    bool HaveData = false;
    int SelectedTile = -1;

    Request currentRequest() const;
    void sourceChanged();
    void render();
    QString describe(QPoint pixel, bool withPixel) const;
};

class BGMapWindow : public DebugTool
{
    Q_OBJECT

public:
    BGMapWindow(DebugBridge* bridge, QWidget* parent = nullptr);

    enum class Kind
    {
        Unavailable,
        ThreeD,
        Text,
        Affine,
        ExtendedTiles,
        ExtendedBitmap,
        ExtendedDirect,
        Large,
    };

    struct MapLayout
    {
        Kind Type = Kind::Unavailable;
        int Width = 0;
        int Height = 0;
        melonDS::u32 MapBase = 0;
        melonDS::u32 MapBytes = 0;
        melonDS::u32 GraphicsBase = 0;
        melonDS::u32 GraphicsBytes = 0;
        bool Colors256 = false;
        bool ExtPalette = false;
        int ExtPaletteSlot = 0;
        bool Wrap = false;
    };

    struct Snapshot
    {
        int Engine = 0;
        int BG = 0;
        melonDS::u32 DispCnt = 0;
        melonDS::u16 BGCnt = 0;
        melonDS::u16 XOffset = 0;
        melonDS::u16 YOffset = 0;
        melonDS::s16 RotA = 0x100;
        melonDS::s16 RotB = 0;
        melonDS::s16 RotC = 0;
        melonDS::s16 RotD = 0x100;
        melonDS::s32 RefX = 0;
        melonDS::s32 RefY = 0;
        melonDS::u8 MosaicWidth = 0;
        melonDS::u8 MosaicHeight = 0;
        MapLayout Layout;
        std::vector<melonDS::u16> Palette;
        std::vector<melonDS::u16> ExtPalette;
        bool ExtPaletteMapped = false;
        std::vector<melonDS::u8> MapData;
        std::vector<melonDS::u8> GraphicsData;
    };

protected:
    void refresh() override;

private:
    QComboBox* Engine;
    QComboBox* BG;
    QSpinBox* Zoom;
    QCheckBox* Viewport;
    QLabel* Params;
    Gfx2DImageView* Image;
    QLabel* Hover;
    Snapshot Data;
    bool HaveData = false;

    void render();
    void drawViewport(QPainter& painter) const;
    QString describe(QPoint pixel) const;
};

class OAMWindow : public DebugTool
{
    Q_OBJECT

public:
    OAMWindow(DebugBridge* bridge, QWidget* parent = nullptr);

    struct Snapshot
    {
        int Engine = 0;
        melonDS::u32 DispCnt = 0;
        std::vector<melonDS::u16> OAM;
        std::vector<melonDS::u16> Palette;
        std::vector<melonDS::u16> ExtPalette;
        bool ExtPaletteMapped = false;
        std::vector<melonDS::u8> VRAM;
    };

protected:
    void refresh() override;

private:
    QComboBox* Engine;
    QSpinBox* Zoom;
    QCheckBox* Transform;
    QLabel* Mapping;
    QTableWidget* Table;
    Gfx2DImageView* Preview;
    QLabel* Details;
    Snapshot Data;
    bool HaveData = false;
    bool ColumnsSized = false;

    void fillTable();
    void renderPreview();
};

#endif
