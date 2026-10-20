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

#ifndef GFX3DSTATEWINDOWS_H
#define GFX3DSTATEWINDOWS_H

#include "DebugTool.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QTableWidget;

struct MatrixSnapshot
{
    melonDS::u32 GXStat;
    melonDS::u32 MatrixMode;
    melonDS::u32 Viewport[6];
    melonDS::s32 ProjStackPointer;
    melonDS::s32 PosStackPointer;
    melonDS::s32 TexStackPointer;
    melonDS::s32 Proj[16];
    melonDS::s32 Pos[16];
    melonDS::s32 Vec[16];
    melonDS::s32 Tex[16];
    melonDS::s32 Clip[16];
    melonDS::s32 ProjStack[16];
    melonDS::s32 PosStack[32][16];
    melonDS::s32 VecStack[32][16];
    melonDS::s32 TexStack[16];
};

struct LightSnapshot
{
    melonDS::s16 Direction[4][3];
    melonDS::u8 Color[4][3];
    melonDS::u8 Diffuse[3];
    melonDS::u8 Ambient[3];
    melonDS::u8 Specular[3];
    melonDS::u8 Emission[3];
    bool UseShininessTable;
    melonDS::u8 Shininess[128];
    melonDS::u32 PolygonAttr;
    melonDS::u32 CurPolygonAttr;
    melonDS::u8 VertexColor[3];
    melonDS::s16 Normal[3];
    melonDS::u32 TexParam;
    melonDS::u32 TexPalette;
};

class MatrixWindow : public DebugTool
{
    Q_OBJECT

public:
    MatrixWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    static constexpr int MatrixKinds = 5;

    QLabel* Cells[MatrixKinds][16];
    QComboBox* Slots[MatrixKinds];
    QLabel* Status;
    QLabel* Mode;
    QLabel* Pointers;
    QLabel* Viewport;
    QCheckBox* HexToggle;
    QCheckBox* AutoToggle;
    QSpinBox* IntervalBox;
    MatrixSnapshot Last {};
    bool HaveLast = false;

    void showState(const MatrixSnapshot& state);
    void showMatrices();
};

class LightWindow : public DebugTool
{
    Q_OBJECT

public:
    LightWindow(DebugBridge* bridge, QWidget* parent = nullptr);

protected:
    void refresh() override;

private:
    QTableWidget* Lights;
    QTableWidget* Materials;
    QTableWidget* Shininess;
    QLabel* ShininessState;
    QLabel* Vertex;
    QLabel* VertexSwatch;
    QLabel* NormalLabel;
    QLabel* PolyAttr;
    QCheckBox* AutoToggle;
    QSpinBox* IntervalBox;
    bool ColumnsSized = false;

    void showState(const LightSnapshot& state);
};

#endif
