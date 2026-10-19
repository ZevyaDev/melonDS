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

#ifndef GFX3DWINDOW_H
#define GFX3DWINDOW_H

#include <memory>
#include <vector>

#include "DebugTool.h"

class QCheckBox;
class QLabel;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTreeWidget;
class Gfx3DWireframe;
class Gfx3DTextureView;

struct Gfx3DVertex
{
    melonDS::s32 X;
    melonDS::s32 Y;
    melonDS::s32 Z;
    melonDS::s32 W;
    melonDS::s32 Clip[4];
    melonDS::u8 Color[3];
    melonDS::s16 TexCoords[2];
    bool Clipped;
};

struct Gfx3DPolygon
{
    melonDS::u32 Attr;
    melonDS::u32 TexParam;
    melonDS::u32 TexPalette;
    melonDS::u32 NumVertices;
    melonDS::s32 Type;
    bool WBuffer;
    bool Degenerate;
    bool FacingView;
    bool Translucent;
    bool IsShadowMask;
    bool IsShadow;
    Gfx3DVertex Vertices[10];
};

struct Gfx3DFrame
{
    std::vector<Gfx3DPolygon> Polygons;
    melonDS::u32 DispCnt;
    std::vector<melonDS::u8> TextureVRAM;
    std::vector<melonDS::u8> PaletteVRAM;
};

class Gfx3DWindow : public DebugTool
{
    Q_OBJECT

public:
    Gfx3DWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QCheckBox* LiveUpdate;
    QTabWidget* Tabs;
    QTreeWidget* PolygonTree;
    QTreeWidget* VertexTree;
    QLabel* PolygonDetail;
    Gfx3DWireframe* Wireframe;
    QSplitter* TexturesPage;
    QTreeWidget* TextureTree;
    Gfx3DTextureView* TextureView;
    QSpinBox* Zoom;
    QCheckBox* IgnoreAlpha;
    QLabel* PaletteView;
    QLabel* TextureDetail;
    QLabel* Info;
    std::shared_ptr<const Gfx3DFrame> Frame;
    std::shared_ptr<const Gfx3DFrame> VRAM;
    int Selected = -1;
    bool PolygonColumnsSized = false;

    void showFrame(const std::shared_ptr<const Gfx3DFrame>& frame);
    void fillPolygons();
    void fillTextures();
    void selectPolygon(int index);
    void showPolygon(int index);
    void showTexture();
};

#endif
