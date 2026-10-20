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

#include <cstring>

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Gfx3DStateWindows.h"
#include "GPU.h"
#include "GPU3D.h"

using namespace melonDS;

constexpr int DefaultIntervalMs = 250;
constexpr double FixedScale = 4096.0;
constexpr double VectorScale = 512.0;
constexpr int PosStackSlots = 32;

enum MatrixKind
{
    mat_Proj,
    mat_Pos,
    mat_Vec,
    mat_Tex,
    mat_Clip,
};

enum LightColumn
{
    light_Index,
    light_Enabled,
    light_X,
    light_Y,
    light_Z,
    light_Raw,
    light_Packed,
    light_Color,
    light_Swatch,
    light_Columns,
};

enum MaterialColumn
{
    material_Name,
    material_Hex,
    material_R,
    material_G,
    material_B,
    material_Swatch,
    material_Columns,
};

static const char* const MatrixNames[5] = {"Projection", "Position", "Direction", "Texture", "Clip"};
static const char* const MatrixIds[5] = {"proj", "pos", "vec", "tex", "clip"};
static const char* const MaterialNames[4] = {"Diffuse", "Ambient", "Specular", "Emission"};

static void SetCell(QTableWidget* table, int row, int column, const QString& text)
{
    QTableWidgetItem* item = table->item(row, column);
    if (!item)
        table->setItem(row, column, new QTableWidgetItem(text));
    else if (item->text() != text)
        item->setText(text);
}

static void SetSwatch(QTableWidget* table, int row, int column, const QColor& color)
{
    QTableWidgetItem* item = table->item(row, column);
    if (!item)
    {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setBackground(color);
}

static QTableWidget* MakeTable(QWidget* parent, int rows, const QStringList& headers)
{
    QTableWidget* table = new QTableWidget(rows, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setFont(MonoFont());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 4);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

static void FitHeight(QTableWidget* table)
{
    int height = table->horizontalHeader()->height() + 2 * table->frameWidth() + 4;
    for (int i = 0; i < table->rowCount(); i++)
        height += table->rowHeight(i);
    table->setFixedHeight(height);
}

static int Expand5(int value)
{
    return (value * 255 + 15) / 31;
}

static QColor Color555(const u8* rgb)
{
    return QColor(Expand5(rgb[0] & 0x1F), Expand5(rgb[1] & 0x1F), Expand5(rgb[2] & 0x1F));
}

static u32 PackBGR555(const u8* rgb)
{
    return (rgb[0] & 0x1F) | ((rgb[1] & 0x1F) << 5) | ((rgb[2] & 0x1F) << 10);
}

static QString Decimal(double value, int places = 4)
{
    return QString::number(value, 'f', places);
}

static QString SwatchStyle(const QColor& color)
{
    return QString("background-color: %1; border: 1px solid palette(mid);").arg(color.name());
}

static QString Fixed(s32 value, bool hex)
{
    if (hex)
        return Hex((u32)value, 8);
    return Decimal(value / FixedScale);
}

static QString Vector(s16 value, bool hex)
{
    if (hex)
        return Hex((u32)value & 0x7FF, 3);
    return Decimal(value / VectorScale);
}

static u32 PackDirection(const s16* dir)
{
    return ((u32)dir[0] & 0x3FF) | (((u32)dir[1] & 0x3FF) << 10) | (((u32)dir[2] & 0x3FF) << 20);
}

static MatrixSnapshot CaptureMatrices(NDS& nds)
{
    const GPU3D& gpu3d = nds.GPU.GPU3D;
    MatrixSnapshot s {};
    s.GXStat = gpu3d.GXStat;
    s.MatrixMode = gpu3d.MatrixMode;
    memcpy(s.Viewport, gpu3d.Viewport, sizeof(s.Viewport));
    s.ProjStackPointer = gpu3d.ProjMatrixStackPointer;
    s.PosStackPointer = gpu3d.PosMatrixStackPointer;
    s.TexStackPointer = gpu3d.TexMatrixStackPointer;
    memcpy(s.Proj, gpu3d.ProjMatrix, sizeof(s.Proj));
    memcpy(s.Pos, gpu3d.PosMatrix, sizeof(s.Pos));
    memcpy(s.Vec, gpu3d.VecMatrix, sizeof(s.Vec));
    memcpy(s.Tex, gpu3d.TexMatrix, sizeof(s.Tex));
    memcpy(s.Clip, gpu3d.ClipMatrix, sizeof(s.Clip));
    memcpy(s.ProjStack, gpu3d.ProjMatrixStack, sizeof(s.ProjStack));
    memcpy(s.PosStack, gpu3d.PosMatrixStack, sizeof(s.PosStack));
    memcpy(s.VecStack, gpu3d.VecMatrixStack, sizeof(s.VecStack));
    memcpy(s.TexStack, gpu3d.TexMatrixStack, sizeof(s.TexStack));
    return s;
}

static LightSnapshot CaptureLights(NDS& nds)
{
    const GPU3D& gpu3d = nds.GPU.GPU3D;
    LightSnapshot s {};
    memcpy(s.Direction, gpu3d.LightDirection, sizeof(s.Direction));
    memcpy(s.Color, gpu3d.LightColor, sizeof(s.Color));
    memcpy(s.Diffuse, gpu3d.MatDiffuse, sizeof(s.Diffuse));
    memcpy(s.Ambient, gpu3d.MatAmbient, sizeof(s.Ambient));
    memcpy(s.Specular, gpu3d.MatSpecular, sizeof(s.Specular));
    memcpy(s.Emission, gpu3d.MatEmission, sizeof(s.Emission));
    s.UseShininessTable = gpu3d.UseShininessTable;
    memcpy(s.Shininess, gpu3d.ShininessTable, sizeof(s.Shininess));
    s.PolygonAttr = gpu3d.PolygonAttr;
    s.CurPolygonAttr = gpu3d.CurPolygonAttr;
    memcpy(s.VertexColor, gpu3d.VertexColor, sizeof(s.VertexColor));
    memcpy(s.Normal, gpu3d.Normal, sizeof(s.Normal));
    s.TexParam = gpu3d.TexParam;
    s.TexPalette = gpu3d.TexPalette;
    return s;
}

static const s32* SelectMatrix(const MatrixSnapshot& s, int kind, int slot)
{
    if (slot <= 0)
    {
        switch (kind)
        {
        case mat_Proj: return s.Proj;
        case mat_Pos: return s.Pos;
        case mat_Vec: return s.Vec;
        case mat_Tex: return s.Tex;
        default: return s.Clip;
        }
    }
    switch (kind)
    {
    case mat_Proj: return s.ProjStack;
    case mat_Pos: return s.PosStack[slot - 1];
    case mat_Vec: return s.VecStack[slot - 1];
    case mat_Tex: return s.TexStack;
    default: return s.Clip;
    }
}

static int StackPointerFor(const MatrixSnapshot& s, int kind)
{
    switch (kind)
    {
    case mat_Proj: return s.ProjStackPointer;
    case mat_Pos:
    case mat_Vec: return s.PosStackPointer;
    case mat_Tex: return s.TexStackPointer;
    default: return -1;
    }
}

static QString ModeName(u32 mode)
{
    static const char* const names[4] = {"projection", "position", "position+vector", "texture"};
    return QString("%1 (%2)").arg(mode).arg(names[mode & 3]);
}

MatrixWindow::MatrixWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Matrices", parent)
{
    setObjectName("matrixWindow");
    resize(760, 700);
    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* bar = new QHBoxLayout();
    HexToggle = new QCheckBox("Raw hex", this);
    HexToggle->setObjectName("matrixHexToggle");
    AutoToggle = new QCheckBox("Auto refresh", this);
    AutoToggle->setObjectName("matrixAutoRefresh");
    AutoToggle->setChecked(true);
    IntervalBox = new QSpinBox(this);
    IntervalBox->setObjectName("matrixInterval");
    IntervalBox->setRange(20, 10000);
    IntervalBox->setSingleStep(50);
    IntervalBox->setSuffix(" ms");
    IntervalBox->setValue(DefaultIntervalMs);
    QPushButton* refreshButton = new QPushButton("Refresh", this);
    refreshButton->setObjectName("matrixRefresh");
    bar->addWidget(HexToggle);
    bar->addWidget(AutoToggle);
    bar->addWidget(IntervalBox);
    bar->addWidget(refreshButton);
    bar->addStretch();
    layout->addLayout(bar);

    Mode = new QLabel(this);
    Mode->setObjectName("matrixMode");
    Pointers = new QLabel(this);
    Pointers->setObjectName("matrixPointers");
    Status = new QLabel(this);
    Status->setObjectName("matrixStatus");
    Viewport = new QLabel(this);
    Viewport->setObjectName("matrixViewport");
    for (QLabel* label : {Mode, Pointers, Status, Viewport})
    {
        label->setFont(MonoFont());
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }

    QGridLayout* grid = new QGridLayout();
    for (int kind = 0; kind < MatrixKinds; kind++)
    {
        QGroupBox* box = new QGroupBox(MatrixNames[kind], this);
        box->setObjectName(QString("%1Group").arg(MatrixIds[kind]));
        QVBoxLayout* boxLayout = new QVBoxLayout(box);
        QComboBox* combo = new QComboBox(box);
        combo->setObjectName(QString("%1Slot").arg(MatrixIds[kind]));
        combo->addItem("Current");
        int slotCount = kind == mat_Pos || kind == mat_Vec ? PosStackSlots : (kind == mat_Clip ? 0 : 1);
        for (int i = 0; i < slotCount; i++)
            combo->addItem(QString("Stack %1").arg(i));
        combo->setVisible(slotCount > 0);
        Slots[kind] = combo;
        boxLayout->addWidget(combo);

        QGridLayout* cells = new QGridLayout();
        cells->setSpacing(2);
        for (int i = 0; i < 16; i++)
        {
            QLabel* cell = new QLabel(box);
            cell->setObjectName(QString("%1Cell%2%3").arg(MatrixIds[kind]).arg(i / 4).arg(i % 4));
            cell->setFont(MonoFont());
            cell->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            cell->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
            cell->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cell->setMinimumWidth(cell->fontMetrics().horizontalAdvance("-000.0000") + 8);
            cells->addWidget(cell, i / 4, i % 4);
            Cells[kind][i] = cell;
        }
        boxLayout->addLayout(cells);
        grid->addWidget(box, kind / 2, kind % 2);
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { showMatrices(); });
    }
    layout->addLayout(grid);
    layout->addStretch();

    connect(HexToggle, &QCheckBox::toggled, this, [this] { showMatrices(); });
    connect(AutoToggle, &QCheckBox::toggled, this, [this](bool on) { setLiveRefresh(on); });
    connect(IntervalBox, &QSpinBox::valueChanged, this, [this](int ms) { setInterval(ms); });
    connect(refreshButton, &QPushButton::clicked, this, &DebugTool::requestRefresh);
    setInterval(DefaultIntervalMs);
}

void MatrixWindow::refresh()
{
    fetch([](NDS& nds) { return CaptureMatrices(nds); },
          [this](const MatrixSnapshot& s) { showState(s); });
}

void MatrixWindow::showState(const MatrixSnapshot& state)
{
    Last = state;
    HaveLast = true;

    Mode->setText(QString("Matrix mode: %1").arg(ModeName(state.MatrixMode)));
    Pointers->setText(QString("Stack pointers: projection %1/1  position/vector %2/31  texture %3/1")
                          .arg(state.ProjStackPointer)
                          .arg(state.PosStackPointer)
                          .arg(state.TexStackPointer));

    bool error = (state.GXStat & (1u << 15)) != 0;
    u32 posLevel = (state.GXStat >> 8) & 0x1F;
    u32 projLevel = (state.GXStat >> 13) & 1;
    bool busy = (state.GXStat & (1u << 14)) != 0;
    Status->setText(QString("GXSTAT %1  position level %2  projection level %3  push/pop busy %4  overflow/underflow %5")
                        .arg(Hex(state.GXStat))
                        .arg(posLevel)
                        .arg(projLevel)
                        .arg(busy ? "yes" : "no")
                        .arg(error ? "SET" : "clear"));
    Status->setStyleSheet(error ? "color: #c00000; font-weight: bold;" : QString());

    Viewport->setText(QString("Viewport: x1=%1 y1=%2 x2=%3 y2=%4 (%5x%6)")
                          .arg(state.Viewport[0])
                          .arg(state.Viewport[1])
                          .arg(state.Viewport[2])
                          .arg(state.Viewport[3])
                          .arg(state.Viewport[4])
                          .arg(state.Viewport[5]));

    for (int kind = 0; kind < MatrixKinds; kind++)
    {
        int sp = StackPointerFor(state, kind);
        QComboBox* combo = Slots[kind];
        for (int i = 1; i < combo->count(); i++)
        {
            QString text = QString("Stack %1").arg(i - 1);
            if (sp == i - 1)
                text += "  <- SP";
            if (combo->itemText(i) != text)
                combo->setItemText(i, text);
        }
    }

    showMatrices();
}

void MatrixWindow::showMatrices()
{
    if (!HaveLast)
        return;
    bool hex = HexToggle->isChecked();
    for (int kind = 0; kind < MatrixKinds; kind++)
    {
        const s32* m = SelectMatrix(Last, kind, Slots[kind]->currentIndex());
        for (int i = 0; i < 16; i++)
        {
            QString text = Fixed(m[i], hex);
            if (Cells[kind][i]->text() != text)
                Cells[kind][i]->setText(text);
        }
    }
}

LightWindow::LightWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "Lights", parent)
{
    setObjectName("lightWindow");
    resize(900, 760);
    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* bar = new QHBoxLayout();
    AutoToggle = new QCheckBox("Auto refresh", this);
    AutoToggle->setObjectName("lightAutoRefresh");
    AutoToggle->setChecked(true);
    IntervalBox = new QSpinBox(this);
    IntervalBox->setObjectName("lightInterval");
    IntervalBox->setRange(20, 10000);
    IntervalBox->setSingleStep(50);
    IntervalBox->setSuffix(" ms");
    IntervalBox->setValue(DefaultIntervalMs);
    QPushButton* refreshButton = new QPushButton("Refresh", this);
    refreshButton->setObjectName("lightRefresh");
    bar->addWidget(AutoToggle);
    bar->addWidget(IntervalBox);
    bar->addWidget(refreshButton);
    bar->addStretch();
    layout->addLayout(bar);

    PolyAttr = new QLabel(this);
    PolyAttr->setObjectName("lightPolygonAttr");
    PolyAttr->setFont(MonoFont());
    PolyAttr->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(PolyAttr);

    QGroupBox* lightBox = new QGroupBox("Lights", this);
    QVBoxLayout* lightLayout = new QVBoxLayout(lightBox);
    Lights = MakeTable(lightBox, 4, {"Light", "Enabled", "X", "Y", "Z", "Raw XYZ (11-bit)", "Packed", "Color BGR555", "Swatch"});
    Lights->setObjectName("lightTable");
    lightLayout->addWidget(Lights);
    layout->addWidget(lightBox);

    QGroupBox* materialBox = new QGroupBox("Material", this);
    QVBoxLayout* materialLayout = new QVBoxLayout(materialBox);
    Materials = MakeTable(materialBox, 4, {"Material", "BGR555", "R", "G", "B", "Swatch"});
    Materials->setObjectName("materialTable");
    materialLayout->addWidget(Materials);
    layout->addWidget(materialBox);

    QGroupBox* vertexBox = new QGroupBox("Current vertex", this);
    QGridLayout* vertexLayout = new QGridLayout(vertexBox);
    Vertex = new QLabel(vertexBox);
    Vertex->setObjectName("vertexColor");
    VertexSwatch = new QLabel(vertexBox);
    VertexSwatch->setObjectName("vertexSwatch");
    VertexSwatch->setFixedSize(32, 16);
    NormalLabel = new QLabel(vertexBox);
    NormalLabel->setObjectName("vertexNormal");
    for (QLabel* label : {Vertex, NormalLabel})
    {
        label->setFont(MonoFont());
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    vertexLayout->addWidget(Vertex, 0, 0);
    vertexLayout->addWidget(VertexSwatch, 0, 1);
    vertexLayout->addWidget(NormalLabel, 1, 0, 1, 2);
    vertexLayout->setColumnStretch(0, 1);
    layout->addWidget(vertexBox);

    QGroupBox* shineBox = new QGroupBox("Shininess table", this);
    QVBoxLayout* shineLayout = new QVBoxLayout(shineBox);
    ShininessState = new QLabel(shineBox);
    ShininessState->setObjectName("shininessState");
    ShininessState->setFont(MonoFont());
    shineLayout->addWidget(ShininessState);
    QStringList columns;
    for (int i = 0; i < 8; i++)
        columns << QString("+%1").arg(i);
    Shininess = MakeTable(shineBox, 16, columns);
    Shininess->setObjectName("shininessTable");
    Shininess->verticalHeader()->show();
    QStringList rows;
    for (int i = 0; i < 16; i++)
        rows << Hex(i * 8, 2);
    Shininess->setVerticalHeaderLabels(rows);
    shineLayout->addWidget(Shininess);
    layout->addWidget(shineBox, 1);

    for (int i = 0; i < 4; i++)
    {
        SetCell(Lights, i, light_Index, QString::number(i));
        SetCell(Materials, i, material_Name, MaterialNames[i]);
    }
    FitHeight(Lights);
    FitHeight(Materials);

    connect(AutoToggle, &QCheckBox::toggled, this, [this](bool on) { setLiveRefresh(on); });
    connect(IntervalBox, &QSpinBox::valueChanged, this, [this](int ms) { setInterval(ms); });
    connect(refreshButton, &QPushButton::clicked, this, &DebugTool::requestRefresh);
    setInterval(DefaultIntervalMs);
}

void LightWindow::refresh()
{
    fetch([](NDS& nds) { return CaptureLights(nds); },
          [this](const LightSnapshot& s) { showState(s); });
}

void LightWindow::showState(const LightSnapshot& state)
{
    QStringList enabledLights;
    for (int i = 0; i < 4; i++)
        if (state.CurPolygonAttr & (1u << i))
            enabledLights << QString::number(i);
    PolyAttr->setText(QString("Polygon attr: current %1  pending %2  lights enabled (current): %3")
                          .arg(Hex(state.CurPolygonAttr))
                          .arg(Hex(state.PolygonAttr))
                          .arg(enabledLights.isEmpty() ? QString("none") : enabledLights.join(",")));

    for (int i = 0; i < 4; i++)
    {
        const s16* dir = state.Direction[i];
        bool enabled = (state.CurPolygonAttr & (1u << i)) != 0;
        SetCell(Lights, i, light_Enabled, enabled ? "on" : "off");
        SetCell(Lights, i, light_X, Vector(dir[0], false));
        SetCell(Lights, i, light_Y, Vector(dir[1], false));
        SetCell(Lights, i, light_Z, Vector(dir[2], false));
        SetCell(Lights, i, light_Raw, QString("%1 %2 %3").arg(Vector(dir[0], true), Vector(dir[1], true), Vector(dir[2], true)));
        SetCell(Lights, i, light_Packed, Hex(PackDirection(dir)));
        SetCell(Lights, i, light_Color, Hex(PackBGR555(state.Color[i]), 4));
        SetSwatch(Lights, i, light_Swatch, Color555(state.Color[i]));
    }

    const u8* materials[4] = {state.Diffuse, state.Ambient, state.Specular, state.Emission};
    for (int i = 0; i < 4; i++)
    {
        SetCell(Materials, i, material_Hex, Hex(PackBGR555(materials[i]), 4));
        SetCell(Materials, i, material_R, QString::number(materials[i][0]));
        SetCell(Materials, i, material_G, QString::number(materials[i][1]));
        SetCell(Materials, i, material_B, QString::number(materials[i][2]));
        SetSwatch(Materials, i, material_Swatch, Color555(materials[i]));
    }

    Vertex->setText(QString("Vertex color: %1  R=%2 G=%3 B=%4")
                        .arg(Hex(PackBGR555(state.VertexColor), 4))
                        .arg(state.VertexColor[0])
                        .arg(state.VertexColor[1])
                        .arg(state.VertexColor[2]));
    VertexSwatch->setStyleSheet(SwatchStyle(Color555(state.VertexColor)));
    NormalLabel->setText(QString("Normal: %1 %2 %3  raw %4 %5 %6")
                             .arg(Vector(state.Normal[0], false), Vector(state.Normal[1], false), Vector(state.Normal[2], false))
                             .arg(Vector(state.Normal[0], true), Vector(state.Normal[1], true), Vector(state.Normal[2], true)));

    ShininessState->setText(QString("Shininess table: %1    TexParam %2  TexPalette %3")
                                .arg(state.UseShininessTable ? "ENABLED" : "disabled")
                                .arg(Hex(state.TexParam))
                                .arg(Hex(state.TexPalette)));
    for (int i = 0; i < 128; i++)
        SetCell(Shininess, i / 8, i % 8, Hex(state.Shininess[i], 2));
    Shininess->setEnabled(state.UseShininessTable);

    if (!ColumnsSized)
    {
        ColumnsSized = true;
        Lights->resizeColumnsToContents();
        Materials->resizeColumnsToContents();
        Shininess->resizeColumnsToContents();
        Lights->horizontalHeader()->setStretchLastSection(true);
    }
}
