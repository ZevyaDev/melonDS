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
#include <functional>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "IORegsWindow.h"
#include "DebugSymbolsQt.h"

using namespace melonDS;

namespace
{

struct IOField
{
    QString Bits;
    QString Name;
    QString Value;
    QString Meaning;
};

struct IODecoded
{
    QString Summary;
    std::vector<IOField> Fields;
};

using IOReader = std::function<u64(NDS&)>;
using IODecoder = std::function<IODecoded(u64)>;

struct IORegister
{
    QString Group;
    u32 Addr = 0;
    int Bits = 32;
    QString Name;
    bool Writable = true;
    bool HoldsAddress = false;
    QString Note;
    IOReader Read;
    IODecoder Decode;

    IORegister& readOnly()
    {
        Writable = false;
        return *this;
    }

    IORegister& address()
    {
        HoldsAddress = true;
        return *this;
    }

    IORegister& note(const QString& text)
    {
        Note = text;
        return *this;
    }
};

struct IRQSource
{
    int Bit;
    const char* Name;
    int CPUs;
};

const IRQSource IRQSources[] = {
    {0, "VBlank", 3},
    {1, "HBlank", 3},
    {2, "VCount match", 3},
    {3, "timer 0", 3},
    {4, "timer 1", 3},
    {5, "timer 2", 3},
    {6, "timer 3", 3},
    {7, "RTC", 2},
    {8, "DMA 0", 3},
    {9, "DMA 1", 3},
    {10, "DMA 2", 3},
    {11, "DMA 3", 3},
    {12, "keypad", 3},
    {13, "GBA slot", 3},
    {16, "IPC sync", 3},
    {17, "IPC send FIFO empty", 3},
    {18, "IPC receive FIFO not empty", 3},
    {19, "cart transfer done", 3},
    {20, "cart IREQ_MC", 3},
    {21, "geometry FIFO", 1},
    {22, "lid open", 2},
    {23, "SPI", 2},
    {24, "wireless", 2},
};

}

static u64 WidthMask(int bits)
{
    return bits >= 64 ? ~(u64)0 : (((u64)1 << bits) - 1);
}

static QString HexWide(u64 value, int bits)
{
    int digits = (bits + 3) / 4;
    if (digits <= 8)
        return Hex((u32)value, digits);
    return Hex((u32)(value >> 32), digits - 8) + Hex((u32)value, 8);
}

static QString BitRange(int low, int high)
{
    return low == high ? QString("bit %1").arg(low) : QString("bits %1-%2").arg(low).arg(high);
}

static QString Number(u64 value)
{
    if (value < 10)
        return QString::number(value);
    return "0x" + QString::number(value, 16).toUpper() + " (" + QString::number(value) + ")";
}

static QString BitNames(u64 bits, const QStringList& names)
{
    QStringList set;
    for (int i = 0; i < names.size(); i++)
        if (bits & ((u64)1 << i))
            set << names[i];
    return set.isEmpty() ? QString("none") : set.join(' ');
}

static QString Sixteenths(u64 value)
{
    return QString("%1/16").arg(std::min<u64>(value, 16));
}

static QString BankName(int bank)
{
    return QString("VRAM %1").arg(QChar('A' + bank));
}

namespace
{

class FieldList
{
public:
    explicit FieldList(u64 raw) : Raw(raw) {}

    u64 bits(int low, int high) const
    {
        return (Raw >> low) & ((((u64)2) << (high - low)) - 1);
    }

    FieldList& raw(const QString& name, const QString& range, const QString& value, const QString& meaning = QString())
    {
        Out.Fields.push_back({range, name, value, meaning});
        return *this;
    }

    FieldList& number(const QString& name, int low, int high, const QString& meaning = QString())
    {
        return raw(name, BitRange(low, high), Number(bits(low, high)), meaning);
    }

    FieldList& flag(const QString& name, int bit, const QString& on = "yes", const QString& off = "no")
    {
        return number(name, bit, bit, bits(bit, bit) ? on : off);
    }

    FieldList& choice(const QString& name, int low, int high, const QStringList& names)
    {
        return number(name, low, high, names.value((qsizetype)bits(low, high), QString("reserved")));
    }

    FieldList& set(const QString& name, int low, int high, const QStringList& names)
    {
        return number(name, low, high, BitNames(bits(low, high), names));
    }

    FieldList& summary(const QString& text)
    {
        Out.Summary = text;
        return *this;
    }

    IODecoded result() const { return Out; }

private:
    u64 Raw;
    IODecoded Out;
};

class RegisterList
{
public:
    explicit RegisterList(int cpu) : CPU(cpu) {}

    void group(const QString& name) { Group = name; }

    IORegister& add(u32 addr, int bits, const QString& name, IOReader read, IODecoder decode = nullptr)
    {
        IORegister reg;
        reg.Group = Group;
        reg.Addr = addr;
        reg.Bits = bits;
        reg.Name = name;
        reg.Read = std::move(read);
        reg.Decode = std::move(decode);
        Registers.push_back(std::move(reg));
        return Registers.back();
    }

    IORegister& io(u32 addr, int bits, const QString& name, IODecoder decode = nullptr);

    int cpu() const { return CPU; }

    std::vector<IORegister> Registers;

private:
    int CPU;
    QString Group;
};

}

static u64 ReadIO(NDS& nds, int cpu, u32 addr, int bits)
{
    switch (bits)
    {
    case 8: return cpu ? nds.ARM7IORead8(addr) : nds.ARM9IORead8(addr);
    case 16: return cpu ? nds.ARM7IORead16(addr) : nds.ARM9IORead16(addr);
    case 32: return cpu ? nds.ARM7IORead32(addr) : nds.ARM9IORead32(addr);
    }
    return ReadIO(nds, cpu, addr, 32) | (ReadIO(nds, cpu, addr + 4, 32) << 32);
}

static void WriteIO(NDS& nds, int cpu, u32 addr, int bits, u64 value)
{
    switch (bits)
    {
    case 8:
        if (cpu)
            nds.ARM7IOWrite8(addr, (u8)value);
        else
            nds.ARM9IOWrite8(addr, (u8)value);
        return;
    case 16:
        if (cpu)
            nds.ARM7IOWrite16(addr, (u16)value);
        else
            nds.ARM9IOWrite16(addr, (u16)value);
        return;
    case 32:
        if (cpu)
            nds.ARM7IOWrite32(addr, (u32)value);
        else
            nds.ARM9IOWrite32(addr, (u32)value);
        return;
    }
    WriteIO(nds, cpu, addr, 32, value & 0xFFFFFFFF);
    WriteIO(nds, cpu, addr + 4, 32, value >> 32);
}

IORegister& RegisterList::io(u32 addr, int bits, const QString& name, IODecoder decode)
{
    int cpu = CPU;
    return add(addr, bits, name, [cpu, addr, bits](NDS& nds) { return ReadIO(nds, cpu, addr, bits); }, std::move(decode));
}

static IODecoded DecodeDispStat(u64 v)
{
    FieldList f(v);
    u64 compare = f.bits(8, 15) | (f.bits(7, 7) << 8);
    f.flag("in VBlank", 0)
        .flag("in HBlank", 1)
        .flag("VCount match", 2)
        .flag("VBlank IRQ", 3, "enabled", "disabled")
        .flag("HBlank IRQ", 4, "enabled", "disabled")
        .flag("VCount IRQ", 5, "enabled", "disabled")
        .raw("VCount compare", "bits 7-15", QString::number(compare), QString("line %1").arg(compare));
    return f.result();
}

static IODecoded DecodeVCount(u64 v)
{
    FieldList f(v);
    f.number("current line", 0, 8, QString("line %1").arg(f.bits(0, 8))).summary(QString("line %1").arg(f.bits(0, 8)));
    return f.result();
}

static IODecoded DecodePowCnt1(u64 v)
{
    FieldList f(v);
    f.flag("LCD", 0, "on", "off")
        .flag("2D engine A", 1, "on", "off")
        .flag("3D rendering", 2, "on", "off")
        .flag("3D geometry", 3, "on", "off")
        .flag("2D engine B", 9, "on", "off")
        .choice("display swap", 15, 15, {"engine A on bottom screen", "engine A on top screen"});
    return f.result();
}

static IODecoded DecodePowCnt2(u64 v)
{
    FieldList f(v);
    f.flag("sound", 0, "on", "off").flag("wireless", 1, "on", "off");
    return f.result();
}

static IODecoded DecodeDispCnt(u64 v, bool engineA)
{
    FieldList f(v);
    QStringList layers = {"BG0", "BG1", "BG2", "BG3", "OBJ"};
    f.choice("BG mode", 0, 2, {"BG0-3 text", "BG0-2 text, BG3 affine", "BG0-1 text, BG2-3 affine", "BG0-2 text, BG3 extended",
                               "BG0-1 text, BG2 affine, BG3 extended", "BG0-1 text, BG2-3 extended",
                               engineA ? "BG0 3D, BG2 large bitmap" : "reserved", "reserved"});
    if (engineA)
        f.choice("BG0 source", 3, 3, {"2D", "3D"});
    f.choice("tile OBJ mapping", 4, 4, {"2D", "1D"})
        .choice("bitmap OBJ 2D width", 5, 5, {"128 dots", "256 dots"})
        .choice("bitmap OBJ mapping", 6, 6, {"2D", "1D"})
        .flag("forced blank", 7)
        .set("layers shown", 8, 12, layers)
        .set("windows", 13, 15, {"WIN0", "WIN1", "OBJWIN"})
        .choice("display mode", 16, 17, {"off", "graphics", engineA ? "VRAM display" : "reserved", engineA ? "main memory display" : "reserved"});
    if (engineA)
        f.number("VRAM display bank", 18, 19, BankName((int)f.bits(18, 19)));
    f.number("tile OBJ 1D boundary", 20, 21, QString("%1 bytes").arg(32 << f.bits(20, 21)));
    if (engineA)
        f.choice("bitmap OBJ 1D boundary", 22, 22, {"128 bytes", "256 bytes"});
    f.flag("OBJ processing in HBlank", 23);
    if (engineA)
    {
        f.number("char base", 24, 26, "+" + Hex((u32)f.bits(24, 26) * 0x10000, 5));
        f.number("screen base", 27, 29, "+" + Hex((u32)f.bits(27, 29) * 0x10000, 5));
    }
    f.flag("BG extended palettes", 30).flag("OBJ extended palettes", 31);
    f.summary(QString("BG mode %1, %2").arg(f.bits(0, 2)).arg(BitNames(f.bits(8, 12), layers)));
    return f.result();
}

static IODecoded DecodeBGCnt(u64 v, int bg)
{
    FieldList f(v);
    f.number("priority", 0, 1)
        .number("char base", 2, 5, "+" + Hex((u32)f.bits(2, 5) * 0x4000, 5))
        .flag("mosaic", 6)
        .choice("colors", 7, 7, {"16x16 (extended: affine tiles)", "256x1 (extended: bitmap)"})
        .number("screen base", 8, 12, "+" + Hex((u32)f.bits(8, 12) * 0x800, 5));
    if (bg < 2)
        f.choice("ext palette slot", 13, 13, {bg ? "slot 1" : "slot 0", bg ? "slot 3" : "slot 2"});
    else
        f.choice("affine overflow", 13, 13, {"transparent", "wraparound"});
    f.choice("screen size", 14, 15, {"text 256x256 / affine 128x128", "text 512x256 / affine 256x256",
                                     "text 256x512 / affine 512x512", "text 512x512 / affine 1024x1024"});
    f.summary(QString("priority %1, char +%2, screen +%3").arg(f.bits(0, 1)).arg(Hex((u32)f.bits(2, 5) * 0x4000, 5)).arg(Hex((u32)f.bits(8, 12) * 0x800, 5)));
    return f.result();
}

static IODecoded DecodeScroll(u64 v)
{
    FieldList f(v);
    f.number("offset", 0, 8).summary(QString::number(f.bits(0, 8)));
    return f.result();
}

static IODecoded DecodeAffineParam(u64 v)
{
    FieldList f(v);
    QString text = QString::number((s16)(u16)v / 256.0, 'f', 4);
    f.number("fixed 8.8", 0, 15, text).summary(text);
    return f.result();
}

static IODecoded DecodeReferencePoint(u64 v)
{
    FieldList f(v);
    s32 value = (s32)((u32)v << 4) >> 4;
    QString text = QString::number(value / 256.0, 'f', 3);
    f.number("fixed 20.8", 0, 27, text).summary(text);
    return f.result();
}

static IODecoded DecodeWindowH(u64 v)
{
    FieldList f(v);
    f.number("right X2", 0, 7).number("left X1", 8, 15).summary(QString("X %1-%2").arg(f.bits(8, 15)).arg(f.bits(0, 7)));
    return f.result();
}

static IODecoded DecodeWindowV(u64 v)
{
    FieldList f(v);
    f.number("bottom Y2", 0, 7).number("top Y1", 8, 15).summary(QString("Y %1-%2").arg(f.bits(8, 15)).arg(f.bits(0, 7)));
    return f.result();
}

static IODecoded DecodeWindowLayers(u64 v, const QString& low, const QString& high)
{
    FieldList f(v);
    QStringList layers = {"BG0", "BG1", "BG2", "BG3", "OBJ", "effects"};
    f.set(low, 0, 5, layers).set(high, 8, 13, layers);
    return f.result();
}

static IODecoded DecodeMosaic(u64 v)
{
    FieldList f(v);
    f.number("BG H size", 0, 3, QString::number(f.bits(0, 3) + 1))
        .number("BG V size", 4, 7, QString::number(f.bits(4, 7) + 1))
        .number("OBJ H size", 8, 11, QString::number(f.bits(8, 11) + 1))
        .number("OBJ V size", 12, 15, QString::number(f.bits(12, 15) + 1));
    return f.result();
}

static IODecoded DecodeBlendCnt(u64 v)
{
    FieldList f(v);
    QStringList targets = {"BG0", "BG1", "BG2", "BG3", "OBJ", "backdrop"};
    QStringList effects = {"none", "alpha blend", "brighten", "darken"};
    f.set("1st target", 0, 5, targets).choice("effect", 6, 7, effects).set("2nd target", 8, 13, targets);
    f.summary(effects[(int)f.bits(6, 7)]);
    return f.result();
}

static IODecoded DecodeBlendAlpha(u64 v)
{
    FieldList f(v);
    f.number("EVA", 0, 4, Sixteenths(f.bits(0, 4))).number("EVB", 8, 12, Sixteenths(f.bits(8, 12)));
    return f.result();
}

static IODecoded DecodeBlendY(u64 v)
{
    FieldList f(v);
    f.number("EVY", 0, 4, Sixteenths(f.bits(0, 4)));
    return f.result();
}

static IODecoded DecodeMasterBright(u64 v)
{
    FieldList f(v);
    QStringList modes = {"off", "brighten", "darken", "reserved"};
    f.number("factor", 0, 4, Sixteenths(f.bits(0, 4))).choice("mode", 14, 15, modes);
    f.summary(QString("%1 %2").arg(modes[(int)f.bits(14, 15)]).arg(Sixteenths(f.bits(0, 4))));
    return f.result();
}

static IODecoded DecodeCaptureCnt(u64 v)
{
    FieldList f(v);
    f.number("EVA", 0, 4, Sixteenths(f.bits(0, 4)))
        .number("EVB", 8, 12, Sixteenths(f.bits(8, 12)))
        .number("write bank", 16, 17, BankName((int)f.bits(16, 17)))
        .number("write offset", 18, 19, "+" + Hex((u32)f.bits(18, 19) * 0x8000, 5))
        .choice("size", 20, 21, {"128x128", "256x64", "256x128", "256x192"})
        .choice("source A", 24, 24, {"graphics screen", "3D screen"})
        .choice("source B", 25, 25, {"VRAM", "main memory FIFO"})
        .number("read offset", 26, 27, "+" + Hex((u32)f.bits(26, 27) * 0x8000, 5))
        .choice("source", 29, 30, {"A", "B", "A+B blended", "A+B blended"})
        .flag("capture", 31, "busy", "idle");
    return f.result();
}

static IODecoded DecodeDisp3DCnt(u64 v)
{
    FieldList f(v);
    f.flag("texture mapping", 0)
        .choice("polygon shading", 1, 1, {"toon", "highlight"})
        .flag("alpha test", 2)
        .flag("alpha blending", 3)
        .flag("anti-aliasing", 4)
        .flag("edge marking", 5)
        .choice("fog mode", 6, 6, {"alpha and color", "alpha only"})
        .flag("fog", 7)
        .number("fog shift", 8, 11)
        .flag("color buffer underflow", 12)
        .flag("polygon/vertex RAM overflow", 13)
        .choice("rear plane", 14, 14, {"clear color", "clear image bitmap"});
    return f.result();
}

static IODecoded DecodeGXStat(u64 v)
{
    FieldList f(v);
    f.flag("box/pos/vec test busy", 0)
        .choice("box test result", 1, 1, {"outside", "inside"})
        .number("position/vector stack level", 8, 12)
        .number("projection stack level", 13, 13)
        .flag("matrix stack busy", 14)
        .flag("matrix stack error", 15)
        .number("command FIFO entries", 16, 24)
        .flag("FIFO less than half full", 25)
        .flag("FIFO empty", 26)
        .flag("geometry engine busy", 27)
        .choice("FIFO IRQ", 30, 31, {"never", "less than half full", "empty", "reserved"});
    f.summary(QString("%1 FIFO entries%2").arg(f.bits(16, 24)).arg(f.bits(27, 27) ? ", busy" : ""));
    return f.result();
}

static IODecoded DecodeRAMCount(u64 v)
{
    FieldList f(v);
    f.number("polygons", 0, 11).number("vertices", 16, 28);
    f.summary(QString("%1 polygons, %2 vertices").arg(f.bits(0, 11)).arg(f.bits(16, 28)));
    return f.result();
}

static void AddColor(FieldList& f)
{
    f.number("red", 0, 4).number("green", 5, 9).number("blue", 10, 14);
}

static IODecoded DecodeClearColor(u64 v)
{
    FieldList f(v);
    AddColor(f);
    f.flag("fog", 15).number("alpha", 16, 20).number("polygon ID", 24, 29);
    return f.result();
}

static IODecoded DecodeClearDepth(u64 v)
{
    FieldList f(v);
    f.number("depth", 0, 14).number("image offset X", 16, 23).number("image offset Y", 24, 31);
    return f.result();
}

static IODecoded DecodeFogColor(u64 v)
{
    FieldList f(v);
    AddColor(f);
    f.number("alpha", 16, 20);
    return f.result();
}

static IODecoded DecodeFogOffset(u64 v)
{
    FieldList f(v);
    f.number("depth", 0, 14);
    return f.result();
}

static IODecoded DecodeAlphaRef(u64 v)
{
    FieldList f(v);
    f.number("alpha", 0, 4);
    return f.result();
}

static IODecoded DecodeMatrixMode(u64 v)
{
    FieldList f(v);
    QStringList modes = {"projection", "position", "position and vector", "texture"};
    f.choice("mode", 0, 1, modes).summary(modes[(int)f.bits(0, 1)]);
    return f.result();
}

static IODecoded DecodePolygonAttr(u64 v)
{
    FieldList f(v);
    f.set("lights", 0, 3, {"0", "1", "2", "3"})
        .choice("mode", 4, 5, {"modulation", "decal", "toon/highlight", "shadow"})
        .flag("back face", 6, "shown", "hidden")
        .flag("front face", 7, "shown", "hidden")
        .flag("translucent depth update", 11)
        .flag("far plane intersecting", 12, "clipped", "hidden")
        .flag("1-dot polygons", 13, "shown", "hidden")
        .choice("depth test", 14, 14, {"less", "equal"})
        .flag("fog", 15)
        .number("alpha", 16, 20)
        .number("polygon ID", 24, 29);
    return f.result();
}

static IODecoded DecodeTexParam(u64 v)
{
    FieldList f(v);
    QStringList formats = {"none", "A3I5", "4-color", "16-color", "256-color", "4x4 compressed", "A5I3", "direct"};
    f.number("VRAM offset", 0, 15, Hex((u32)f.bits(0, 15) * 8, 5))
        .flag("repeat S", 16)
        .flag("repeat T", 17)
        .flag("flip S", 18)
        .flag("flip T", 19)
        .number("width", 20, 22, QString::number(8 << f.bits(20, 22)))
        .number("height", 23, 25, QString::number(8 << f.bits(23, 25)))
        .choice("format", 26, 28, formats)
        .choice("color 0", 29, 29, {"shown", "transparent"})
        .choice("texcoord transform", 30, 31, {"none", "texcoord", "normal", "vertex"});
    f.summary(QString("%1 %2x%3 at %4").arg(formats[(int)f.bits(26, 28)]).arg(8 << f.bits(20, 22)).arg(8 << f.bits(23, 25)).arg(Hex((u32)f.bits(0, 15) * 8, 5)));
    return f.result();
}

static IODecoded DecodePaletteBase(u64 v)
{
    FieldList f(v);
    f.number("base", 0, 12, Hex((u32)f.bits(0, 12) * 16, 5) + " (4-color: " + Hex((u32)f.bits(0, 12) * 8, 5) + ")");
    return f.result();
}

static IODecoded DecodeViewport(u64 v)
{
    FieldList f(v);
    f.number("X1", 0, 7).number("Y1", 8, 15).number("X2", 16, 23).number("Y2", 24, 31);
    f.summary(QString("(%1,%2)-(%3,%4)").arg(f.bits(0, 7)).arg(f.bits(8, 15)).arg(f.bits(16, 23)).arg(f.bits(24, 31)));
    return f.result();
}

static IODecoded DecodeDMACnt(u64 v, int cpu, int channel)
{
    FieldList f(v);
    int countHigh = cpu == 0 ? 20 : (channel == 3 ? 15 : 13);
    QStringList timings;
    if (cpu == 0)
        timings = QStringList{"immediately", "VBlank", "HBlank", "display sync", "main memory display", "DS cart slot", "GBA cart slot", "geometry FIFO"};
    else
        timings = QStringList{"immediately", "VBlank", "DS cart slot", (channel & 1) ? "GBA cart slot" : "wireless"};
    u64 count = f.bits(0, countHigh);
    f.number("word count", 0, countHigh, count ? QString() : QString("0 = maximum"))
        .choice("destination control", 21, 22, {"increment", "decrement", "fixed", "increment/reload"})
        .choice("source control", 23, 24, {"increment", "decrement", "fixed", "prohibited"})
        .flag("repeat", 25)
        .choice("transfer size", 26, 26, {"16-bit", "32-bit"});
    int timingLow = cpu == 0 ? 27 : 28;
    QString timing = timings.value((qsizetype)f.bits(timingLow, 29));
    f.choice("start timing", timingLow, 29, timings).flag("IRQ at end", 30).flag("enable", 31);
    if (f.bits(31, 31))
        f.summary(QString("on, %1, %2 x %3").arg(timing).arg(count ? count : (u64)1 << (countHigh + 1)).arg(f.bits(26, 26) ? "32-bit" : "16-bit"));
    else
        f.summary("off");
    return f.result();
}

static IODecoded DecodeTimerCounter(u64 v)
{
    FieldList f(v);
    QString reload = Hex((u32)f.bits(32, 47), 4);
    f.number("counter", 0, 15).raw("reload", "write-only", reload).summary("reload " + reload);
    return f.result();
}

static IODecoded DecodeTimerControl(u64 v)
{
    FieldList f(v);
    QStringList prescalers = {"F/1 (33.51 MHz)", "F/64", "F/256", "F/1024"};
    f.choice("prescaler", 0, 1, prescalers).flag("count-up", 2).flag("IRQ on overflow", 6).flag("start", 7);
    if (f.bits(7, 7))
        f.summary("running, " + (f.bits(2, 2) ? QString("count-up") : prescalers[(int)f.bits(0, 1)]));
    else
        f.summary("stopped");
    return f.result();
}

static IODecoded DecodeIPCSync(u64 v)
{
    FieldList f(v);
    f.number("data in", 0, 3).number("data out", 8, 11).flag("IRQ from remote", 14, "enabled", "disabled");
    f.summary(QString("in %1, out %2").arg(f.bits(0, 3)).arg(f.bits(8, 11)));
    return f.result();
}

static IODecoded DecodeIPCFifoCnt(u64 v)
{
    FieldList f(v);
    f.flag("send FIFO empty", 0)
        .flag("send FIFO full", 1)
        .flag("send empty IRQ", 2, "enabled", "disabled")
        .flag("receive FIFO empty", 8)
        .flag("receive FIFO full", 9)
        .flag("receive not empty IRQ", 10, "enabled", "disabled")
        .flag("error", 14)
        .flag("FIFO enable", 15);
    return f.result();
}

static IODecoded DecodeIME(u64 v)
{
    FieldList f(v);
    f.flag("master enable", 0, "enabled", "disabled").summary(f.bits(0, 0) ? "enabled" : "disabled");
    return f.result();
}

static IODecoded DecodeIRQBits(u64 v, int cpu, const QString& on)
{
    FieldList f(v);
    QStringList active;
    for (const IRQSource& source : IRQSources)
    {
        if (!(source.CPUs & (1 << cpu)))
            continue;
        f.flag(source.Name, source.Bit, on, "-");
        if (f.bits(source.Bit, source.Bit))
            active << source.Name;
    }
    f.summary(active.join(", "));
    return f.result();
}

static IODecoded DecodeKeyInput(u64 v)
{
    FieldList f(v);
    QStringList keys = {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"};
    QStringList pressed;
    for (int i = 0; i < keys.size(); i++)
    {
        f.flag(keys[i], i, "released", "pressed");
        if (!f.bits(i, i))
            pressed << keys[i];
    }
    f.summary(pressed.isEmpty() ? QString("none pressed") : pressed.join(' '));
    return f.result();
}

static IODecoded DecodeKeyCnt(u64 v)
{
    FieldList f(v);
    f.set("keys", 0, 9, {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"})
        .flag("IRQ", 14, "enabled", "disabled")
        .choice("condition", 15, 15, {"any key (OR)", "all keys (AND)"});
    return f.result();
}

static IODecoded DecodeExtKeyIn(u64 v)
{
    FieldList f(v);
    f.flag("X", 0, "released", "pressed")
        .flag("Y", 1, "released", "pressed")
        .flag("debug", 3, "released", "pressed")
        .flag("pen", 6, "up", "down")
        .flag("hinge", 7, "closed", "open");
    QStringList state;
    if (!f.bits(0, 0))
        state << "X";
    if (!f.bits(1, 1))
        state << "Y";
    if (!f.bits(6, 6))
        state << "pen down";
    if (f.bits(7, 7))
        state << "lid closed";
    f.summary(state.join(' '));
    return f.result();
}

static IODecoded DecodeExMemCnt(u64 v)
{
    FieldList f(v);
    QStringList waits = {"10 cycles", "8 cycles", "6 cycles", "18 cycles"};
    f.choice("GBA SRAM access", 0, 1, waits)
        .choice("GBA ROM 1st access", 2, 3, waits)
        .choice("GBA ROM 2nd access", 4, 4, {"6 cycles", "4 cycles"})
        .choice("PHI output", 5, 6, {"low", "4.19 MHz", "8.38 MHz", "16.76 MHz"})
        .choice("GBA slot owner", 7, 7, {"ARM9", "ARM7"})
        .choice("DS slot owner", 11, 11, {"ARM9", "ARM7"})
        .choice("main memory mode", 14, 14, {"async", "sync"})
        .choice("main memory priority", 15, 15, {"ARM9", "ARM7"});
    f.summary(QString("GBA slot %1, DS slot %2").arg(f.bits(7, 7) ? "ARM7" : "ARM9").arg(f.bits(11, 11) ? "ARM7" : "ARM9"));
    return f.result();
}

static QString VRAMMapping(int bank, u64 cnt)
{
    static const u32 LCDCBase[9] = {0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000, 0x06890000, 0x06894000, 0x06898000, 0x068A0000};
    if (!(cnt & 0x80))
        return "disabled";
    u32 mst = cnt & ((bank >= 2 && bank <= 6) ? 7 : 3);
    u32 ofs = (cnt >> 3) & 3;
    if (mst == 0)
        return "LCDC " + Hex(LCDCBase[bank]);
    switch (bank)
    {
    case 0:
    case 1:
        if (mst == 1)
            return "engine A BG " + Hex(0x06000000 + 0x20000 * ofs);
        if (mst == 2)
            return "engine A OBJ " + Hex(0x06400000 + 0x20000 * (ofs & 1));
        return QString("texture slot %1").arg(ofs);
    case 2:
    case 3:
        if (mst == 1)
            return "engine A BG " + Hex(0x06000000 + 0x20000 * ofs);
        if (mst == 2)
            return "ARM7 " + Hex(0x06000000 + 0x20000 * (ofs & 1));
        if (mst == 3)
            return QString("texture slot %1").arg(ofs);
        if (mst == 4)
            return bank == 2 ? "engine B BG 06200000" : "engine B OBJ 06600000";
        break;
    case 4:
        if (mst == 1)
            return "engine A BG 06000000";
        if (mst == 2)
            return "engine A OBJ 06400000";
        if (mst == 3)
            return "texture palette slots 0-3";
        if (mst == 4)
            return "engine A BG ext palette slots 0-3";
        break;
    case 5:
    case 6:
        if (mst == 1)
            return "engine A BG " + Hex(0x06000000 + 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1));
        if (mst == 2)
            return "engine A OBJ " + Hex(0x06400000 + 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1));
        if (mst == 3)
            return QString("texture palette slot %1").arg((ofs & 1) + (ofs >> 1) * 4);
        if (mst == 4)
            return (ofs & 1) ? "engine A BG ext palette slots 2-3" : "engine A BG ext palette slots 0-1";
        if (mst == 5)
            return "engine A OBJ ext palette";
        break;
    case 7:
        if (mst == 1)
            return "engine B BG 06200000";
        if (mst == 2)
            return "engine B BG ext palette slots 0-3";
        break;
    case 8:
        if (mst == 1)
            return "engine B BG 06208000";
        if (mst == 2)
            return "engine B OBJ 06600000";
        if (mst == 3)
            return "engine B OBJ ext palette";
        break;
    }
    return "invalid";
}

static IODecoded DecodeVRAMCnt(u64 v, int bank)
{
    FieldList f(v);
    bool wideMST = bank >= 2 && bank <= 6;
    bool hasOffset = bank <= 3 || bank == 5 || bank == 6;
    QString mapping = VRAMMapping(bank, v);
    f.number("MST", 0, wideMST ? 2 : 1, mapping);
    if (hasOffset)
        f.number("offset", 3, 4);
    f.flag("enable", 7).summary(mapping);
    return f.result();
}

static IODecoded DecodeWRAMCnt(u64 v)
{
    FieldList f(v);
    QStringList layouts = {"ARM9 32K", "ARM9 2nd 16K, ARM7 1st 16K", "ARM9 1st 16K, ARM7 2nd 16K", "ARM7 32K"};
    f.choice("shared WRAM", 0, 1, layouts).summary(layouts[(int)f.bits(0, 1)]);
    return f.result();
}

static IODecoded DecodeVRAMStat(u64 v)
{
    FieldList f(v);
    f.flag("VRAM C at ARM7", 0).flag("VRAM D at ARM7", 1);
    return f.result();
}

static IODecoded DecodePostFlag(u64 v)
{
    FieldList f(v);
    f.flag("boot done", 0);
    return f.result();
}

static IODecoded DecodeDivCnt(u64 v)
{
    FieldList f(v);
    QStringList modes = {"32/32", "64/32", "64/64", "64/32"};
    f.choice("mode", 0, 1, modes).flag("division by zero", 14).flag("busy", 15);
    f.summary(modes[(int)f.bits(0, 1)] + (f.bits(15, 15) ? ", busy" : ""));
    return f.result();
}

static IODecoded DecodeSigned(u64 v)
{
    FieldList f(v);
    f.summary(QString::number((qint64)v));
    return f.result();
}

static IODecoded DecodeUnsigned(u64 v)
{
    FieldList f(v);
    f.summary(QString::number(v));
    return f.result();
}

static IODecoded DecodeSqrtCnt(u64 v)
{
    FieldList f(v);
    f.choice("mode", 0, 0, {"32-bit", "64-bit"}).flag("busy", 15);
    return f.result();
}

static IODecoded DecodeAuxSPICnt(u64 v)
{
    FieldList f(v);
    f.choice("baud rate", 0, 1, {"4 MHz", "2 MHz", "1 MHz", "512 kHz"})
        .flag("chipselect hold", 6)
        .flag("busy", 7)
        .choice("mode", 13, 13, {"ROM", "backup SPI"})
        .flag("transfer IRQ", 14, "enabled", "disabled")
        .flag("slot enable", 15);
    return f.result();
}

static IODecoded DecodeROMCtrl(u64 v)
{
    FieldList f(v);
    u64 block = f.bits(24, 26);
    QString size = block == 0 ? QString("none") : block == 7 ? QString("4 bytes") : QString("0x%1 bytes").arg(0x100 << block, 0, 16);
    f.number("KEY1 gap1 length", 0, 12)
        .flag("KEY2 data", 13)
        .flag("SE", 14)
        .flag("KEY2 apply seed", 15)
        .number("KEY1 gap2 length", 16, 21)
        .flag("KEY2 command", 22)
        .flag("data word ready", 23)
        .number("block size", 24, 26, size)
        .choice("transfer clock", 27, 27, {"6.7 MHz", "4.2 MHz"})
        .flag("KEY1 gap clocks", 28)
        .flag("RESB release", 29)
        .choice("direction", 30, 30, {"read", "write"})
        .flag("busy", 31);
    f.summary(f.bits(31, 31) ? "busy, " + size : QString("idle"));
    return f.result();
}

static IODecoded DecodeSoundCnt(u64 v)
{
    FieldList f(v);
    QStringList formats = {"PCM8", "PCM16", "IMA-ADPCM", "PSG/noise"};
    u64 pan = f.bits(16, 22);
    QString panText = pan == 64 ? QString("center") : pan < 64 ? QString("left %1").arg(64 - pan) : QString("right %1").arg(pan - 64);
    f.number("volume", 0, 6)
        .choice("volume divider", 8, 9, {"/1", "/2", "/4", "/16"})
        .flag("hold", 15)
        .number("panning", 16, 22, panText)
        .number("PSG duty", 24, 26, QString("%1/8").arg(f.bits(24, 26) + 1))
        .choice("repeat", 27, 28, {"manual", "loop", "one-shot", "prohibited"})
        .choice("format", 29, 30, formats)
        .flag("busy", 31);
    if (f.bits(31, 31))
        f.summary(QString("on, %1, volume %2").arg(formats[(int)f.bits(29, 30)]).arg(f.bits(0, 6)));
    else
        f.summary("off");
    return f.result();
}

static IODecoded DecodeSoundMaster(u64 v)
{
    FieldList f(v);
    f.number("master volume", 0, 6)
        .choice("left output", 8, 9, {"left mixer", "ch1", "ch3", "ch1+ch3"})
        .choice("right output", 10, 11, {"right mixer", "ch1", "ch3", "ch1+ch3"})
        .flag("ch1 to mixer", 12, "no", "yes")
        .flag("ch3 to mixer", 13, "no", "yes")
        .flag("master enable", 15);
    f.summary(f.bits(15, 15) ? QString("on, volume %1").arg(f.bits(0, 6)) : QString("off"));
    return f.result();
}

static IODecoded DecodeSoundBias(u64 v)
{
    FieldList f(v);
    f.number("bias", 0, 9);
    return f.result();
}

static IODecoded DecodeCaptureControl(u64 v)
{
    FieldList f(v);
    for (int unit = 0; unit < 2; unit++)
    {
        int b = unit * 8;
        QString name = QString("capture %1 ").arg(unit);
        f.flag(name + (unit ? "add ch3 to ch2" : "add ch1 to ch0"), b)
            .choice(name + "source", b + 1, b + 1, {unit ? "right mixer" : "left mixer", unit ? "ch2" : "ch0"})
            .choice(name + "repeat", b + 2, b + 2, {"loop", "one-shot"})
            .choice(name + "format", b + 3, b + 3, {"PCM16", "PCM8"})
            .flag(name + "busy", b + 7);
    }
    return f.result();
}

static IODecoded DecodeSPICnt(u64 v)
{
    FieldList f(v);
    QStringList devices = {"power manager", "firmware", "touchscreen", "reserved"};
    f.choice("baud rate", 0, 1, {"4 MHz", "2 MHz", "1 MHz", "512 kHz"})
        .flag("busy", 7)
        .choice("device", 8, 9, devices)
        .choice("transfer size", 10, 10, {"8-bit", "16-bit"})
        .flag("chipselect hold", 11)
        .flag("IRQ", 14, "enabled", "disabled")
        .flag("SPI enable", 15);
    f.summary(devices[(int)f.bits(8, 9)] + (f.bits(7, 7) ? ", busy" : ""));
    return f.result();
}

static IODecoded DecodeRTCPort(u64 v)
{
    FieldList f(v);
    f.flag("data", 0, "1", "0")
        .flag("clock", 1, "1", "0")
        .flag("select", 2, "1", "0")
        .choice("data direction", 4, 4, {"read", "write"})
        .choice("clock direction", 5, 5, {"read", "write"})
        .choice("select direction", 6, 6, {"read", "write"});
    return f.result();
}

static IODecoded DecodeRTCStatus1(u64 v)
{
    FieldList f(v);
    f.choice("hour mode", 1, 1, {"12-hour", "24-hour"})
        .number("general purpose", 2, 3)
        .flag("INT1 flag", 4)
        .flag("INT2 flag", 5)
        .flag("power low", 6)
        .flag("power off", 7);
    return f.result();
}

static IODecoded DecodeRTCStatus2(u64 v)
{
    FieldList f(v);
    f.number("INT1 mode", 0, 3).flag("INT2 enable", 6).flag("test mode", 7);
    return f.result();
}

static IODecoded DecodeRTCDateTime(u64 v)
{
    FieldList f(v);
    QStringList names = {"year", "month", "day", "weekday", "hour", "minute", "second"};
    for (int i = 0; i < names.size(); i++)
        f.raw(names[i], QString("byte %1").arg(i), Hex((u32)f.bits(i * 8, i * 8 + 7), 2));
    auto bcd = [&f](int index, u32 mask) { return Hex((u32)f.bits(index * 8, index * 8 + 7) & mask, 2); };
    f.summary(QString("20%1-%2-%3 %4:%5:%6%7").arg(bcd(0, 0xFF)).arg(bcd(1, 0x1F)).arg(bcd(2, 0x3F))
        .arg(bcd(4, 0x3F)).arg(bcd(5, 0x7F)).arg(bcd(6, 0x7F)).arg(f.bits(38, 38) ? " PM" : ""));
    return f.result();
}

static GPU2D& Engine(NDS& nds, int engine)
{
    return engine ? nds.GPU.GPU2D_B : nds.GPU.GPU2D_A;
}

static u64 ReadGXStat(NDS& nds)
{
    GPU3D& gx = nds.GPU.GPU3D;
    u32 level = gx.CmdFIFO.Level();
    return gx.GXStat | ((gx.PosMatrixStackPointer & 0x1F) << 8) | ((gx.ProjMatrixStackPointer & 0x1) << 13) | (level << 16)
        | (level < 128 ? (1 << 25) : 0) | (level == 0 ? (1 << 26) : 0);
}

static u64 ReadViewport(NDS& nds)
{
    const u32* vp = nds.GPU.GPU3D.Viewport;
    return vp[0] | (((191 - vp[1]) & 0xFF) << 8) | (vp[2] << 16) | (((191 - vp[3]) & 0xFF) << 24);
}

static void AddLCD(RegisterList& list)
{
    int cpu = list.cpu();
    list.add(0x04000004, 16, "DISPSTAT", [cpu](NDS& nds) -> u64 { return nds.GPU.DispStat[cpu]; }, DecodeDispStat);
    list.add(0x04000006, 16, "VCOUNT", [](NDS& nds) -> u64 { return nds.GPU.VCount; }, DecodeVCount)
        .note("changes the scanline being drawn");
    if (cpu == 0)
        list.add(0x04000304, 16, "POWCNT1", [](NDS& nds) -> u64 { return nds.PowerControl9; }, DecodePowCnt1);
    else
    {
        list.io(0x04000304, 16, "POWCNT2", DecodePowCnt2);
        list.io(0x04000206, 16, "WIFIWAITCNT");
    }
}

static void AddEngine(RegisterList& list, int engine)
{
    u32 base = engine ? 0x04001000 : 0x04000000;
    list.add(base, 32, "DISPCNT", [engine](NDS& nds) -> u64 { return Engine(nds, engine).DispCnt; },
        [engine](u64 v) { return DecodeDispCnt(v, engine == 0); });
    for (int bg = 0; bg < 4; bg++)
        list.add(base + 0x08 + bg * 2, 16, QString("BG%1CNT").arg(bg), [engine, bg](NDS& nds) -> u64 { return Engine(nds, engine).BGCnt[bg]; },
            [bg](u64 v) { return DecodeBGCnt(v, bg); });
    for (int bg = 0; bg < 4; bg++)
    {
        list.add(base + 0x10 + bg * 4, 16, QString("BG%1HOFS").arg(bg), [engine, bg](NDS& nds) -> u64 { return Engine(nds, engine).BGXPos[bg]; }, DecodeScroll);
        list.add(base + 0x12 + bg * 4, 16, QString("BG%1VOFS").arg(bg), [engine, bg](NDS& nds) -> u64 { return Engine(nds, engine).BGYPos[bg]; }, DecodeScroll);
    }
    for (int i = 0; i < 2; i++)
    {
        u32 at = base + 0x20 + i * 0x10;
        int bg = i + 2;
        list.add(at + 0x0, 16, QString("BG%1PA").arg(bg), [engine, i](NDS& nds) -> u64 { return (u16)Engine(nds, engine).BGRotA[i]; }, DecodeAffineParam);
        list.add(at + 0x2, 16, QString("BG%1PB").arg(bg), [engine, i](NDS& nds) -> u64 { return (u16)Engine(nds, engine).BGRotB[i]; }, DecodeAffineParam);
        list.add(at + 0x4, 16, QString("BG%1PC").arg(bg), [engine, i](NDS& nds) -> u64 { return (u16)Engine(nds, engine).BGRotC[i]; }, DecodeAffineParam);
        list.add(at + 0x6, 16, QString("BG%1PD").arg(bg), [engine, i](NDS& nds) -> u64 { return (u16)Engine(nds, engine).BGRotD[i]; }, DecodeAffineParam);
        list.add(at + 0x8, 32, QString("BG%1X").arg(bg), [engine, i](NDS& nds) -> u64 { return (u32)Engine(nds, engine).BGXRef[i] & 0x0FFFFFFF; }, DecodeReferencePoint);
        list.add(at + 0xC, 32, QString("BG%1Y").arg(bg), [engine, i](NDS& nds) -> u64 { return (u32)Engine(nds, engine).BGYRef[i] & 0x0FFFFFFF; }, DecodeReferencePoint);
    }
    list.add(base + 0x40, 16, "WIN0H", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return (e.Win0Coords[0] << 8) | e.Win0Coords[1];
    }, DecodeWindowH);
    list.add(base + 0x42, 16, "WIN1H", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return (e.Win1Coords[0] << 8) | e.Win1Coords[1];
    }, DecodeWindowH);
    list.add(base + 0x44, 16, "WIN0V", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return (e.Win0Coords[2] << 8) | e.Win0Coords[3];
    }, DecodeWindowV);
    list.add(base + 0x46, 16, "WIN1V", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return (e.Win1Coords[2] << 8) | e.Win1Coords[3];
    }, DecodeWindowV);
    list.add(base + 0x48, 16, "WININ", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return e.WinCnt[0] | (e.WinCnt[1] << 8);
    }, [](u64 v) { return DecodeWindowLayers(v, "window 0", "window 1"); });
    list.add(base + 0x4A, 16, "WINOUT", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return e.WinCnt[2] | (e.WinCnt[3] << 8);
    }, [](u64 v) { return DecodeWindowLayers(v, "outside", "OBJ window"); });
    list.add(base + 0x4C, 16, "MOSAIC", [engine](NDS& nds) -> u64 {
        GPU2D& e = Engine(nds, engine);
        return e.BGMosaicSize[0] | (e.BGMosaicSize[1] << 4) | (e.OBJMosaicSize[0] << 8) | (e.OBJMosaicSize[1] << 12);
    }, DecodeMosaic);
    list.add(base + 0x50, 16, "BLDCNT", [engine](NDS& nds) -> u64 { return Engine(nds, engine).BlendCnt; }, DecodeBlendCnt);
    list.add(base + 0x52, 16, "BLDALPHA", [engine](NDS& nds) -> u64 { return Engine(nds, engine).BlendAlpha; }, DecodeBlendAlpha);
    list.add(base + 0x54, 16, "BLDY", [engine](NDS& nds) -> u64 { return Engine(nds, engine).EVY; }, DecodeBlendY);
    if (engine == 0)
        list.add(0x04000064, 32, "DISPCAPCNT", [](NDS& nds) -> u64 { return nds.GPU.CaptureCnt; }, DecodeCaptureCnt);
    list.add(base + 0x6C, 16, "MASTER_BRIGHT", [engine](NDS& nds) -> u64 {
        return engine ? nds.GPU.MasterBrightnessB : nds.GPU.MasterBrightnessA;
    }, DecodeMasterBright);
}

static void Add3D(RegisterList& list)
{
    list.add(0x04000060, 16, "DISP3DCNT", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.DispCnt; }, DecodeDisp3DCnt)
        .note("bits 12-13 = 1 acknowledge the overflow flags");
    list.add(0x04000600, 32, "GXSTAT", ReadGXStat, DecodeGXStat)
        .note("bit 15 = 1 acknowledges a matrix stack error; bits 30-31 select the FIFO IRQ");
    list.add(0x04000604, 32, "RAM_COUNT", [](NDS& nds) -> u64 {
        return nds.GPU.GPU3D.NumPolygons | ((u64)nds.GPU.GPU3D.NumVertices << 16);
    }, DecodeRAMCount).readOnly();
    list.add(0x04000340, 16, "ALPHA_TEST_REF", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.AlphaRefVal; }, DecodeAlphaRef);
    list.add(0x04000350, 32, "CLEAR_COLOR", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.ClearAttr1; }, DecodeClearColor);
    list.add(0x04000354, 32, "CLEAR_DEPTH", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.ClearAttr2; }, DecodeClearDepth);
    list.add(0x04000358, 32, "FOG_COLOR", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.FogColor; }, DecodeFogColor);
    list.add(0x0400035C, 16, "FOG_OFFSET", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.FogOffset; }, DecodeFogOffset);
    list.add(0x04000440, 32, "MTX_MODE", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.MatrixMode; }, DecodeMatrixMode).readOnly();
    list.add(0x040004A4, 32, "POLYGON_ATTR", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.PolygonAttr; }, DecodePolygonAttr).readOnly();
    list.add(0x040004A8, 32, "TEXIMAGE_PARAM", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.TexParam; }, DecodeTexParam).readOnly();
    list.add(0x040004AC, 32, "PLTT_BASE", [](NDS& nds) -> u64 { return nds.GPU.GPU3D.TexPalette; }, DecodePaletteBase).readOnly();
    list.add(0x04000580, 32, "VIEWPORT", ReadViewport, DecodeViewport).readOnly();
}

static void AddSound(RegisterList& list)
{
    list.group("Sound");
    list.io(0x04000500, 16, "SOUNDCNT", DecodeSoundMaster);
    list.io(0x04000504, 16, "SOUNDBIAS", DecodeSoundBias);
    list.io(0x04000508, 16, "SNDCAPCNT", DecodeCaptureControl).note("setting bit 7 or 15 starts a capture");
    list.io(0x04000510, 32, "SNDCAP0DAD").address();
    list.io(0x04000518, 32, "SNDCAP1DAD").address();
    list.group("Sound channels");
    for (int ch = 0; ch < 16; ch++)
        list.io(0x04000400 + ch * 0x10, 32, QString("SOUND%1CNT").arg(ch), DecodeSoundCnt).note("setting bit 31 starts the channel");
}

static void AddSerial(RegisterList& list)
{
    list.group("SPI");
    list.io(0x040001C0, 16, "SPICNT", DecodeSPICnt);
    list.io(0x040001C2, 16, "SPIDATA").readOnly();
    list.group("RTC");
    list.io(0x04000138, 16, "RTC", DecodeRTCPort).readOnly();
    list.add(0, 8, "RTC status 1", [](NDS& nds) -> u64 {
        melonDS::RTC::StateData state;
        nds.RTC.GetState(state);
        return state.StatusReg1;
    }, DecodeRTCStatus1).readOnly();
    list.add(0, 8, "RTC status 2", [](NDS& nds) -> u64 {
        melonDS::RTC::StateData state;
        nds.RTC.GetState(state);
        return state.StatusReg2;
    }, DecodeRTCStatus2).readOnly();
    list.add(0, 56, "RTC date/time", [](NDS& nds) -> u64 {
        melonDS::RTC::StateData state;
        nds.RTC.GetState(state);
        u64 packed = 0;
        for (int i = 0; i < 7; i++)
            packed |= (u64)state.DateTime[i] << (i * 8);
        return packed;
    }, DecodeRTCDateTime).readOnly();
}

static void AddDMA(RegisterList& list)
{
    int cpu = list.cpu();
    list.group("DMA");
    for (int ch = 0; ch < 4; ch++)
    {
        u32 at = 0x040000B0 + ch * 12;
        list.io(at, 32, QString("DMA%1SAD").arg(ch)).address();
        list.io(at + 4, 32, QString("DMA%1DAD").arg(ch)).address();
        list.io(at + 8, 32, QString("DMA%1CNT").arg(ch), [cpu, ch](u64 v) { return DecodeDMACnt(v, cpu, ch); })
            .note("setting bit 31 starts the transfer");
    }
    if (cpu == 0)
        for (int ch = 0; ch < 4; ch++)
            list.io(0x040000E0 + ch * 4, 32, QString("DMA%1FILL").arg(ch));
}

static void AddTimers(RegisterList& list)
{
    int cpu = list.cpu();
    list.group("Timers");
    for (int i = 0; i < 4; i++)
    {
        int id = cpu * 4 + i;
        list.add(0x04000100 + i * 4, 16, QString("TM%1CNT_L").arg(i), [id](NDS& nds) -> u64 {
            return ((nds.Timers[id].Counter >> 10) & 0xFFFF) | ((u64)nds.Timers[id].Reload << 32);
        }, DecodeTimerCounter).note("a write sets the reload value");
        list.add(0x04000102 + i * 4, 16, QString("TM%1CNT_H").arg(i), [id](NDS& nds) -> u64 { return nds.Timers[id].Cnt; }, DecodeTimerControl);
    }
}

static void AddSystem(RegisterList& list)
{
    int cpu = list.cpu();
    list.group("IPC");
    list.io(0x04000180, 16, "IPCSYNC", DecodeIPCSync).note("bit 13 = 1 sends an IRQ to the other CPU");
    list.io(0x04000184, 16, "IPCFIFOCNT", DecodeIPCFifoCnt).note("bit 3 = 1 clears the send FIFO; bit 14 = 1 acknowledges an error");

    list.group("Interrupts");
    list.add(0x04000208, 16, "IME", [cpu](NDS& nds) -> u64 { return nds.IME[cpu]; }, DecodeIME);
    list.add(0x04000210, 32, "IE", [cpu](NDS& nds) -> u64 { return nds.IE[cpu]; }, [cpu](u64 v) { return DecodeIRQBits(v, cpu, "enabled"); });
    list.add(0x04000214, 32, "IF", [cpu](NDS& nds) -> u64 { return nds.IF[cpu]; }, [cpu](u64 v) { return DecodeIRQBits(v, cpu, "pending"); })
        .note("1 bits acknowledge (clear) those interrupts");

    list.group("Keypad");
    list.add(0x04000130, 16, "KEYINPUT", [](NDS& nds) -> u64 { return nds.KeyInput & 0xFFFF; }, DecodeKeyInput).readOnly();
    list.io(0x04000132, 16, "KEYCNT", DecodeKeyCnt);
    if (cpu)
        list.add(0x04000136, 16, "EXTKEYIN", [](NDS& nds) -> u64 { return (nds.KeyInput >> 16) & 0xFFFF; }, DecodeExtKeyIn).readOnly();

    list.group("Memory control");
    list.add(0x04000204, 16, cpu ? "EXMEMSTAT" : "EXMEMCNT", [cpu](NDS& nds) -> u64 { return nds.ExMemCnt[cpu]; }, DecodeExMemCnt);
    if (cpu == 0)
    {
        for (int bank = 0; bank < 9; bank++)
        {
            u32 addr = 0x04000240 + bank + (bank >= 7 ? 1 : 0);
            list.add(addr, 8, QString("VRAMCNT_%1").arg(QChar('A' + bank)), [bank](NDS& nds) -> u64 { return nds.GPU.VRAMCNT[bank]; },
                [bank](u64 v) { return DecodeVRAMCnt(v, bank); });
            if (bank == 6)
                list.io(0x04000247, 8, "WRAMCNT", DecodeWRAMCnt);
        }
    }
    else
    {
        list.add(0x04000240, 8, "VRAMSTAT", [](NDS& nds) -> u64 { return nds.GPU.VRAMSTAT; }, DecodeVRAMStat).readOnly();
        list.io(0x04000241, 8, "WRAMSTAT", DecodeWRAMCnt).readOnly();
    }
    list.io(0x04000300, 8, "POSTFLG", DecodePostFlag);
    if (cpu)
        list.io(0x04000308, 16, "BIOSPROT").readOnly();
}

static void AddMath(RegisterList& list)
{
    list.group("Math");
    list.io(0x04000280, 16, "DIVCNT", DecodeDivCnt).note("a write starts a division");
    list.io(0x04000290, 64, "DIV_NUMER", DecodeSigned).note("a write starts a division");
    list.io(0x04000298, 64, "DIV_DENOM", DecodeSigned).note("a write starts a division");
    list.io(0x040002A0, 64, "DIV_RESULT", DecodeSigned).readOnly();
    list.io(0x040002A8, 64, "DIVREM_RESULT", DecodeSigned).readOnly();
    list.io(0x040002B0, 16, "SQRTCNT", DecodeSqrtCnt).note("a write starts a square root");
    list.io(0x040002B4, 32, "SQRT_RESULT", DecodeUnsigned).readOnly();
    list.io(0x040002B8, 64, "SQRT_PARAM", DecodeUnsigned).note("a write starts a square root");
}

static void AddCart(RegisterList& list)
{
    list.group("Cartridge");
    list.io(0x040001A0, 16, "AUXSPICNT", DecodeAuxSPICnt);
    list.io(0x040001A2, 16, "AUXSPIDATA").readOnly();
    list.io(0x040001A4, 32, "ROMCTRL", DecodeROMCtrl).readOnly();
}

static std::vector<IORegister> BuildRegisters(int cpu)
{
    RegisterList list(cpu);
    list.group("LCD and power");
    AddLCD(list);
    if (cpu == 0)
    {
        list.group("Display engine A");
        AddEngine(list, 0);
        list.group("Display engine B");
        AddEngine(list, 1);
        list.group("3D engine");
        Add3D(list);
    }
    else
    {
        AddSound(list);
        AddSerial(list);
    }
    AddDMA(list);
    AddTimers(list);
    AddSystem(list);
    if (cpu == 0)
        AddMath(list);
    AddCart(list);
    return std::move(list.Registers);
}

static const std::vector<IORegister>& Registers(int cpu)
{
    static const std::vector<IORegister> arm9 = BuildRegisters(0);
    static const std::vector<IORegister> arm7 = BuildRegisters(1);
    return cpu ? arm7 : arm9;
}

static void Paint(QTreeWidgetItem* item, const QColor& color)
{
    item->setForeground(2, color);
    item->setForeground(3, color);
}

IORegsWindow::IORegsWindow(DebugBridge* bridge, QWidget* parent) : DebugTool(bridge, "IO registers", parent)
{
    resize(860, 720);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* row = new QHBoxLayout();
    CPU = new QComboBox(this);
    CPU->addItems({"ARM9", "ARM7"});
    Filter = new QLineEdit(this);
    Filter->setPlaceholderText("name or address");
    Filter->setClearButtonEnabled(true);
    AutoRefresh = new QCheckBox("Auto refresh", this);
    AutoRefresh->setChecked(true);
    QPushButton* refreshNow = new QPushButton("Refresh", this);
    row->addWidget(new QLabel("CPU", this));
    row->addWidget(CPU);
    row->addWidget(new QLabel("Filter", this));
    row->addWidget(Filter, 1);
    row->addWidget(AutoRefresh);
    row->addWidget(refreshNow);
    layout->addLayout(row);

    Tree = new QTreeWidget(this);
    Tree->setHeaderLabels({"Address / bits", "Name", "Value", "Meaning"});
    Tree->setFont(MonoFont());
    Tree->setUniformRowHeights(true);
    Tree->setExpandsOnDoubleClick(false);
    QFontMetrics metrics(Tree->font());
    Tree->setColumnWidth(0, metrics.horizontalAdvance(QString(12, 'M')) + Tree->indentation() * 2);
    Tree->setColumnWidth(1, metrics.horizontalAdvance(QString(30, 'M')));
    Tree->setColumnWidth(2, metrics.horizontalAdvance(QString(22, 'M')));
    layout->addWidget(Tree);
    Info = new QLabel("Double-click a register to write it. Values that changed since the last refresh are red.", this);
    layout->addWidget(Info);

    connect(CPU, &QComboBox::currentIndexChanged, this, [this] {
        rebuild();
        requestRefresh();
    });
    connect(Filter, &QLineEdit::textChanged, this, &IORegsWindow::applyFilter);
    connect(AutoRefresh, &QCheckBox::toggled, this, [this](bool on) { setLiveRefresh(on); });
    connect(refreshNow, &QPushButton::clicked, this, &IORegsWindow::requestRefresh);
    connect(Tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) { editRegister(item); });
    setInterval(500);
    rebuild();
}

void IORegsWindow::rebuild()
{
    Tree->clear();
    Rows.clear();
    Previous.clear();
    HavePrevious = false;
    const std::vector<IORegister>& regs = Registers(CPU->currentIndex());
    QFont bold = Tree->font();
    bold.setBold(true);
    QString group;
    for (size_t i = 0; i < regs.size(); i++)
    {
        const IORegister& reg = regs[i];
        if (reg.Group != group)
        {
            group = reg.Group;
            QTreeWidgetItem* header = new QTreeWidgetItem(Tree, QStringList{group});
            header->setFirstColumnSpanned(true);
            header->setFont(0, bold);
            header->setFlags(Qt::ItemIsEnabled);
        }
        QTreeWidgetItem* item = new QTreeWidgetItem(Tree, {reg.Addr ? Hex(reg.Addr) : QString("-"), reg.Name, "", ""});
        item->setData(0, Qt::UserRole, (int)i);
        if (!reg.Writable)
            item->setToolTip(1, "read-only here");
        else if (!reg.Note.isEmpty())
            item->setToolTip(1, reg.Note);
        Rows.push_back(item);
    }
    applyFilter();
}

void IORegsWindow::applyFilter()
{
    QString text = Filter->text().trimmed();
    if (text.startsWith("0x", Qt::CaseInsensitive))
        text = text.mid(2);
    const std::vector<IORegister>& regs = Registers(CPU->currentIndex());
    QTreeWidgetItem* header = nullptr;
    bool anyShown = false;
    for (int i = 0; i < Tree->topLevelItemCount(); i++)
    {
        QTreeWidgetItem* item = Tree->topLevelItem(i);
        QVariant index = item->data(0, Qt::UserRole);
        if (!index.isValid())
        {
            if (header)
                header->setHidden(!anyShown);
            header = item;
            anyShown = false;
            continue;
        }
        const IORegister& reg = regs[index.toInt()];
        bool match = text.isEmpty()
            || reg.Name.contains(text, Qt::CaseInsensitive)
            || reg.Group.contains(text, Qt::CaseInsensitive)
            || (reg.Addr && Hex(reg.Addr).contains(text, Qt::CaseInsensitive));
        item->setHidden(!match);
        anyShown |= match;
    }
    if (header)
        header->setHidden(!anyShown);
}

void IORegsWindow::refresh()
{
    int cpu = CPU->currentIndex();
    const std::vector<IORegister>* regs = &Registers(cpu);
    fetch([regs](NDS& nds) {
        std::vector<u64> values;
        values.reserve(regs->size());
        for (const IORegister& reg : *regs)
            values.push_back(reg.Read(nds));
        return values;
    }, [this, cpu](const std::vector<u64>& values) {
        if (cpu != CPU->currentIndex())
        {
            refresh();
            return;
        }
        showValues(values);
    });
}

void IORegsWindow::showValues(const std::vector<u64>& values)
{
    if (values.size() != Rows.size())
        return;
    const std::vector<IORegister>& regs = Registers(CPU->currentIndex());
    DebugSymbols& syms = Bridge->symbols();
    QColor changedColor(220, 50, 50);
    QColor normalColor = Tree->palette().text().color();
    for (size_t i = 0; i < Rows.size(); i++)
    {
        const IORegister& reg = regs[i];
        QTreeWidgetItem* item = Rows[i];
        bool changed = HavePrevious && values[i] != Previous[i];
        if (HavePrevious && !changed)
        {
            Paint(item, normalColor);
            for (int c = 0; c < item->childCount(); c++)
                Paint(item->child(c), normalColor);
            continue;
        }
        u64 shown = values[i] & WidthMask(reg.Bits);
        IODecoded decoded = reg.Decode ? reg.Decode(values[i]) : IODecoded();
        item->setText(2, HexWide(shown, reg.Bits));
        item->setText(3, reg.HoldsAddress ? SymStr(syms.describe((u32)shown)) : decoded.Summary);
        Paint(item, changed ? changedColor : normalColor);
        if (item->childCount() != (int)decoded.Fields.size())
        {
            qDeleteAll(item->takeChildren());
            for (const IOField& field : decoded.Fields)
                new QTreeWidgetItem(item, {field.Bits, field.Name, field.Value, field.Meaning});
            continue;
        }
        for (int c = 0; c < item->childCount(); c++)
        {
            QTreeWidgetItem* child = item->child(c);
            const IOField& field = decoded.Fields[c];
            bool fieldChanged = changed && (child->text(2) != field.Value || child->text(3) != field.Meaning);
            child->setText(2, field.Value);
            child->setText(3, field.Meaning);
            Paint(child, fieldChanged ? changedColor : normalColor);
        }
    }
    Previous = values;
    HavePrevious = true;
}

void IORegsWindow::editRegister(QTreeWidgetItem* item)
{
    while (item->parent())
        item = item->parent();
    QVariant index = item->data(0, Qt::UserRole);
    if (!index.isValid())
        return;
    int cpu = CPU->currentIndex();
    size_t row = index.toInt();
    const IORegister& reg = Registers(cpu)[row];
    if (!reg.Writable)
    {
        Info->setText(reg.Name + " is read-only here.");
        return;
    }
    u64 current = HavePrevious && row < Previous.size() ? Previous[row] & WidthMask(reg.Bits) : 0;
    QString prompt = QString("%1 %2 at %3, %4-bit hex").arg(cpu ? "ARM7" : "ARM9").arg(reg.Name).arg(Hex(reg.Addr)).arg(reg.Bits);
    if (!reg.Note.isEmpty())
        prompt += "\n" + reg.Note;
    bool ok;
    QString text = QInputDialog::getText(this, "Write IO register", prompt, QLineEdit::Normal, HexWide(current, reg.Bits), &ok);
    if (!ok)
        return;
    u64 value = text.trimmed().remove("0x", Qt::CaseInsensitive).toULongLong(&ok, 16);
    if (!ok)
    {
        Info->setText("Not a hex value: " + text);
        return;
    }
    value &= WidthMask(reg.Bits);
    u32 addr = reg.Addr;
    int bits = reg.Bits;
    Bridge->post([cpu, addr, bits, value](NDS& nds) { WriteIO(nds, cpu, addr, bits, value); });
    Info->setText(QString("Wrote %1 to %2.").arg(HexWide(value, bits)).arg(reg.Name));
    requestRefresh();
}
