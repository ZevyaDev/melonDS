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
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include "lua/LuaDesmume.h"
#include "lua/LuaMain.h"
#include "debugger/DebugBridge.h"
#include "debugger/FrameTools.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "Window.h"
#include "Screen.h"
#include "ARM.h"
#include "GPU.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Savestate.h"

using namespace melonDS;

namespace LuaDesmume
{

static const char* const MainKey = "desmume.main";
static const char* const StateMeta = "desmume.savestate";
static const char* const FileDataKey = "desmume.filedata";
static const char* const OpacityKey = "desmume.opacity";

enum Callback
{
    cb_Before,
    cb_After,
    cb_Gui,
    cb_Start,
    cb_Exit,
    cb_Save,
    cb_Load,
    cb_Count,
};

static const char* const CallbackKeys[cb_Count] = {
    "desmume.before", "desmume.after", "desmume.gui", "desmume.start", "desmume.exit", "desmume.save", "desmume.load",
};

static EmuInstance* Instance(lua_State* L)
{
    return get_bundle(L)->getEmuInstance();
}

static FrameTools& Frames(lua_State* L)
{
    return Instance(L)->getDebugBridge()->frames();
}

static void Print(lua_State* L, const QString& text)
{
    get_bundle(L)->printText(text);
}

static void Exec(lua_State* L, const std::function<void(NDS&)>& fn)
{
    if (!Instance(L)->getDebugBridge()->run(fn))
        luaL_error(L, "no console is running, or the emulator thread did not answer");
}

static bool SafeIO(int cpu, u32 addr)
{
    if ((addr & 0xFFFFFF00) == 0x04100000 || (addr & 0xFFFFFF00) == 0x04004200)
        return false;
    if (!cpu && (addr & ~3u) == 0x04000600)
        return false;
    if (cpu && addr >= 0x04800000 && addr < 0x04810000)
    {
        u32 reg = addr & 0x7FFE;
        if (reg == 0x044 || reg == 0x060)
            return false;
    }
    return true;
}

static u32 BusRead(NDS& nds, int cpu, u32 addr, int size)
{
    if ((addr >> 24) != 0x04)
        return nds.Debugger.Peek(cpu, addr, size);
    addr &= ~(u32)(size - 1);
    if (!SafeIO(cpu, addr))
        return 0;
    bool lag = nds.LagFrameFlag;
    u32 value;
    if (cpu)
        value = size == 1 ? nds.ARM7Read8(addr) : size == 2 ? nds.ARM7Read16(addr) : nds.ARM7Read32(addr);
    else
        value = size == 1 ? nds.ARM9Read8(addr) : size == 2 ? nds.ARM9Read16(addr) : nds.ARM9Read32(addr);
    nds.LagFrameFlag = lag;
    return value;
}

static int CPUOf(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return 0;
    if (lua_isnumber(L, idx))
    {
        lua_Integer n = lua_tointeger(L, idx);
        return n == 1 || n == 7;
    }
    QString name = QString::fromUtf8(lua_tostring(L, idx)).toLower();
    return name.contains("arm7") || name == "sub";
}

static int NativeRead(lua_State* L)
{
    u32 addr = (u32)luaL_checkinteger(L, 1);
    int size = (int)luaL_checkinteger(L, 2);
    bool sign = lua_toboolean(L, 3);
    int cpu = CPUOf(L, 4);
    u32 value = 0;
    Exec(L, [&](NDS& nds) { value = BusRead(nds, cpu, addr, size); });
    if (sign)
        lua_pushinteger(L, size == 1 ? (s8)value : size == 2 ? (s16)value : (s32)value);
    else
        lua_pushinteger(L, value);
    return 1;
}

static int NativeWrite(lua_State* L)
{
    u32 addr = (u32)luaL_checkinteger(L, 1);
    int size = (int)luaL_checkinteger(L, 2);
    u32 value = (u32)(lua_Integer)luaL_checknumber(L, 3);
    int cpu = CPUOf(L, 4);
    Exec(L, [&](NDS& nds) {
        if ((addr >> 24) != 0x04)
        {
            nds.Debugger.Poke(cpu, addr, size, value);
            return;
        }
        u32 aligned = addr & ~(u32)(size - 1);
        if (cpu)
        {
            if (size == 1) nds.ARM7Write8(aligned, value);
            else if (size == 2) nds.ARM7Write16(aligned, value);
            else nds.ARM7Write32(aligned, value);
        }
        else
        {
            if (size == 1) nds.ARM9Write8(aligned, value);
            else if (size == 2) nds.ARM9Write16(aligned, value);
            else nds.ARM9Write32(aligned, value);
        }
    });
    return 0;
}

static int NativeRange(lua_State* L)
{
    u32 addr = (u32)luaL_checkinteger(L, 1);
    u32 length = (u32)luaL_checkinteger(L, 2);
    int cpu = CPUOf(L, 3);
    if (length > 0x1000000)
        return luaL_argerror(L, 2, "range too large");
    std::vector<u8> data(length);
    Exec(L, [&](NDS& nds) {
        for (u32 i = 0; i < length; i++)
            data[i] = BusRead(nds, cpu, addr + i, 1);
    });
    lua_createtable(L, length, 0);
    for (u32 i = 0; i < length; i++)
    {
        lua_pushinteger(L, data[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static u32* RegisterSlot(ARM& arm, const std::string& name)
{
    if (name == "cpsr")
        return &arm.CPSR;
    if (name == "spsr")
    {
        switch (arm.CPSR & 0x1F)
        {
        case 0x11: return &arm.R_FIQ[7];
        case 0x12: return &arm.R_IRQ[2];
        case 0x13: return &arm.R_SVC[2];
        case 0x17: return &arm.R_ABT[2];
        case 0x1B: return &arm.R_UND[2];
        }
        return nullptr;
    }
    std::string reg = name;
    if (reg == "sp") reg = "r13";
    else if (reg == "lr") reg = "r14";
    else if (reg == "pc") reg = "r15";
    if (reg.size() < 2 || reg[0] != 'r')
        return nullptr;
    int index = atoi(reg.c_str() + 1);
    if (index < 0 || index > 15 || std::to_string(index) != reg.substr(1))
        return nullptr;
    return &arm.R[index];
}

static int NativeGetRegister(lua_State* L)
{
    int cpu = CPUOf(L, 1);
    std::string name = luaL_checkstring(L, 2);
    bool found = false;
    u32 value = 0;
    Exec(L, [&](NDS& nds) {
        ARM& arm = cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
        if (u32* slot = RegisterSlot(arm, name))
        {
            found = true;
            value = *slot;
        }
    });
    if (!found)
        lua_pushnil(L);
    else
        lua_pushinteger(L, value);
    return 1;
}

static int NativeSetRegister(lua_State* L)
{
    int cpu = CPUOf(L, 1);
    std::string name = luaL_checkstring(L, 2);
    u32 value = (u32)(lua_Integer)luaL_checknumber(L, 3);
    bool found = false;
    Exec(L, [&](NDS& nds) {
        ARM& arm = cpu ? (ARM&)*nds.ARM7 : (ARM&)*nds.ARM9;
        u32* slot = RegisterSlot(arm, name);
        found = slot;
        if (slot == &arm.R[15])
            arm.JumpTo(value | ((arm.CPSR & 0x20) ? 1 : 0));
        else if (slot == &arm.CPSR)
        {
            u32 pc = arm.R[15] - ((arm.CPSR & 0x20) ? 2 : 4);
            u32 old = arm.CPSR;
            arm.CPSR = value;
            arm.UpdateMode(old, value);
            if ((old ^ value) & 0x20)
                arm.JumpTo(pc | ((value & 0x20) ? 1 : 0));
        }
        else if (slot)
            *slot = value;
    });
    if (!found)
        return luaL_argerror(L, 1, "unknown register");
    return 0;
}

static int NativePrint(lua_State* L)
{
    Print(L, QString::fromUtf8(luaL_checkstring(L, 1)));
    return 0;
}

static int SetCallback(lua_State* L, int which, int idx)
{
    if (!lua_isnoneornil(L, idx))
        luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, CallbackKeys[which]);
    if (lua_isnoneornil(L, idx))
        lua_pushnil(L);
    else
        lua_pushvalue(L, idx);
    lua_setfield(L, LUA_REGISTRYINDEX, CallbackKeys[which]);
    return 1;
}

static void RunCallback(lua_State* L, int which)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, CallbackKeys[which]) != LUA_TFUNCTION)
    {
        lua_pop(L, 1);
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
    {
        Print(L, QString("lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, CallbackKeys[which]);
    }
}

static int Resume(lua_State* L, QString& error)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, MainKey) != LUA_TTHREAD)
    {
        lua_pop(L, 1);
        return 0;
    }
    lua_State* co = lua_tothread(L, -1);
    lua_pop(L, 1);
    int results = 0;
    int status = lua_resume(co, L, 0, &results);
    if (status == LUA_YIELD)
    {
        lua_pop(co, results);
        return 1;
    }
    if (status != LUA_OK)
    {
        luaL_traceback(L, co, lua_tostring(co, -1), 0);
        error = QString::fromUtf8(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, MainKey);
    return status == LUA_OK ? 0 : -1;
}

static int EmuFrameAdvance(lua_State* L)
{
    if (!lua_isyieldable(L))
        return luaL_error(L, "emu.frameadvance works in the script's main body, not inside a callback or the console");
    return lua_yield(L, 0);
}

static int EmuFrameCount(lua_State* L)
{
    lua_pushinteger(L, Frames(L).frameCount());
    return 1;
}

static int EmuLagCount(lua_State* L)
{
    lua_pushinteger(L, Frames(L).lagCount());
    return 1;
}

static int EmuLagged(lua_State* L)
{
    lua_pushboolean(L, Frames(L).lagged());
    return 1;
}

static int EmuEmulating(lua_State* L)
{
    lua_pushboolean(L, Instance(L)->emuIsActive());
    return 1;
}

static int EmuAtFrameBoundary(lua_State* L)
{
    lua_pushboolean(L, Frames(L).atBoundary() || lua_isyieldable(L));
    return 1;
}

static int EmuRegisterBefore(lua_State* L) { return SetCallback(L, cb_Before, 1); }
static int EmuRegisterAfter(lua_State* L) { return SetCallback(L, cb_After, 1); }
static int EmuRegisterStart(lua_State* L) { return SetCallback(L, cb_Start, 1); }
static int EmuRegisterExit(lua_State* L) { return SetCallback(L, cb_Exit, 1); }
static int GuiRegister(lua_State* L) { return SetCallback(L, cb_Gui, 1); }
static int StateRegisterSave(lua_State* L) { return SetCallback(L, cb_Save, 1); }
static int StateRegisterLoad(lua_State* L) { return SetCallback(L, cb_Load, 1); }

static void OnGuiThread(lua_State* L, std::function<void(EmuThread*)> fn)
{
    EmuThread* thread = get_bundle(L)->getEmuThread();
    QMetaObject::invokeMethod(qApp, [thread, fn] { fn(thread); }, Qt::QueuedConnection);
}

static int EmuPause(lua_State* L)
{
    OnGuiThread(L, [](EmuThread* t) { t->emuPause(); });
    return 0;
}

static int EmuUnpause(lua_State* L)
{
    OnGuiThread(L, [](EmuThread* t) { t->emuUnpause(); });
    return 0;
}

static int EmuReset(lua_State* L)
{
    OnGuiThread(L, [](EmuThread* t) { t->emuReset(); });
    return 0;
}

static int EmuMessage(lua_State* L)
{
    Instance(L)->osdAddMessage(0, "%s", luaL_checkstring(L, 1));
    return 0;
}

static int EmuGameCode(lua_State* L, int length)
{
    char code[5] = {};
    Exec(L, [&](NDS& nds) {
        if (const NDSCart::CartCommon* cart = nds.GetNDSCart())
            memcpy(code, cart->GetHeader().GameCode, 4);
    });
    code[length] = 0;
    lua_pushstring(L, code);
    return 1;
}

static int EmuGameCode4(lua_State* L) { return EmuGameCode(L, 4); }
static int EmuGameCode3(lua_State* L) { return EmuGameCode(L, 3); }

static int EmuEmulateFrame(lua_State* L)
{
    if (!Frames(L).emulateFrame())
        return luaL_error(L, "emu.emulateframe works only in emu.registerbefore/registerafter or gui.register callbacks");
    return 0;
}

static int EmuNothing(lua_State* L)
{
    return 0;
}

static const struct
{
    const char* Name;
    u32 Value;
} ColorNames[] = {
    {"white", 0xFFFFFFFF}, {"black", 0x000000FF}, {"clear", 0x00000000}, {"transparent", 0x00000000},
    {"gray", 0x7F7F7FFF}, {"grey", 0x7F7F7FFF}, {"red", 0xFF0000FF}, {"orange", 0xFF7F00FF},
    {"yellow", 0xFFFF00FF}, {"chartreuse", 0x7FFF00FF}, {"green", 0x00FF00FF}, {"teal", 0x00FF7FFF},
    {"cyan", 0x00FFFFFF}, {"blue", 0x0000FFFF}, {"purple", 0x7F00FFFF}, {"magenta", 0xFF00FFFF},
};

static u32 ColorArg(lua_State* L, int idx, u32 def)
{
    switch (lua_type(L, idx))
    {
    case LUA_TNUMBER:
        return (u32)(lua_Integer)lua_tonumber(L, idx);
    case LUA_TSTRING:
    {
        QString text = QString::fromUtf8(lua_tostring(L, idx)).trimmed().toLower();
        if (text.startsWith('#'))
        {
            bool ok = false;
            u32 value = text.mid(1).toUInt(&ok, 16);
            if (ok && text.size() == 7)
                return (value << 8) | 0xFF;
            if (ok && text.size() == 9)
                return value;
        }
        for (const auto& c : ColorNames)
            if (text == c.Name)
                return c.Value;
        luaL_error(L, "unknown color %s", lua_tostring(L, idx));
        return def;
    }
    case LUA_TTABLE:
    {
        int parts[4] = {255, 255, 255, 255};
        static const char* const keys[4] = {"r", "g", "b", "a"};
        for (int i = 0; i < 4; i++)
        {
            if (lua_getfield(L, idx, keys[i]) == LUA_TNIL)
            {
                lua_pop(L, 1);
                lua_rawgeti(L, idx, i + 1);
            }
            if (lua_isnumber(L, -1))
                parts[i] = (int)lua_tointeger(L, -1) & 0xFF;
            lua_pop(L, 1);
        }
        return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    }
    default:
        return def;
    }
}

static double Opacity(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, OpacityKey);
    double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 1.0;
    lua_pop(L, 1);
    return value;
}

static QColor ToColor(lua_State* L, u32 rgba)
{
    int alpha = (int)((rgba & 0xFF) * Opacity(L));
    return QColor(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, std::clamp(alpha, 0, 255));
}

static QFont GuiFont()
{
    QFont font("Courier New");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(8);
    return font;
}

static int GuiText(lua_State* L)
{
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2) + 192;
    QColor fore = ToColor(L, ColorArg(L, 4, 0xFFFFFFFF));
    QColor back = ToColor(L, ColorArg(L, 5, 0x000000FF));
    size_t length = 0;
    const char* str = luaL_tolstring(L, 3, &length);
    QString text = QString::fromUtf8(str, (int)length);
    Frames(L).paintLua([&](QPainter& p) {
        QFont font = GuiFont();
        QFontMetrics metrics(font);
        int line = y + metrics.ascent();
        for (const QString& row : text.split('\n'))
        {
            QPainterPath path;
            path.addText(x + 1, line, font, row);
            if (back.alpha())
                p.strokePath(path, QPen(back, 2));
            p.fillPath(path, fore);
            line += metrics.height();
        }
    });
    return 0;
}

static int GuiBox(lua_State* L)
{
    int x1 = (int)luaL_checknumber(L, 1), y1 = (int)luaL_checknumber(L, 2) + 192;
    int x2 = (int)luaL_checknumber(L, 3), y2 = (int)luaL_checknumber(L, 4) + 192;
    u32 fill = ColorArg(L, 5, 0xFFFFFF3F);
    u32 outline = ColorArg(L, 6, fill | 0xFF);
    QColor fillColor = ToColor(L, fill), lineColor = ToColor(L, outline);
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    Frames(L).paintLua([&](QPainter& p) {
        p.fillRect(QRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1), fillColor);
        p.setPen(QPen(lineColor, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(x1, y1, x2 - x1, y2 - y1));
    });
    return 0;
}

static int GuiLine(lua_State* L)
{
    int x1 = (int)luaL_checknumber(L, 1), y1 = (int)luaL_checknumber(L, 2) + 192;
    int x2 = (int)luaL_checknumber(L, 3), y2 = (int)luaL_checknumber(L, 4) + 192;
    QColor color = ToColor(L, ColorArg(L, 5, 0xFFFFFFFF));
    Frames(L).paintLua([&](QPainter& p) {
        p.setPen(QPen(color, 1));
        p.drawLine(x1, y1, x2, y2);
    });
    return 0;
}

static int GuiPixel(lua_State* L)
{
    int x = (int)luaL_checknumber(L, 1), y = (int)luaL_checknumber(L, 2) + 192;
    QColor color = ToColor(L, ColorArg(L, 3, 0xFFFFFFFF));
    Frames(L).paintLua([&](QPainter& p) { p.fillRect(QRect(x, y, 1, 1), color); });
    return 0;
}

static bool ScreenPixel(NDS& nds, int x, int y, u32& out)
{
    void* top = nullptr;
    void* bottom = nullptr;
    if (x < 0 || x >= 256 || y < -192 || y >= 192 || !nds.GPU.GetFramebuffers(&top, &bottom) || !top || !bottom)
        return false;
    const u32* buffer = (const u32*)(y < 0 ? top : bottom);
    out = buffer[(y < 0 ? y + 192 : y) * 256 + x];
    return true;
}

static int GuiGetPixel(lua_State* L)
{
    int x = (int)luaL_checknumber(L, 1), y = (int)luaL_checknumber(L, 2);
    u32 pixel = 0;
    bool ok = false;
    Exec(L, [&](NDS& nds) { ok = ScreenPixel(nds, x, y, pixel); });
    QRgb gui = Frames(L).luaPixel(x, y + 192);
    int r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
    int a = qAlpha(gui);
    if (a)
    {
        r = (qRed(gui) * 255 + r * (255 - a)) / 255;
        g = (qGreen(gui) * 255 + g * (255 - a)) / 255;
        b = (qBlue(gui) * 255 + b * (255 - a)) / 255;
    }
    lua_pushinteger(L, ok || a ? r : 0);
    lua_pushinteger(L, ok || a ? g : 0);
    lua_pushinteger(L, ok || a ? b : 0);
    lua_pushinteger(L, 255);
    return 4;
}

static int GuiOpacity(lua_State* L)
{
    lua_pushnumber(L, std::max(0.0, (double)luaL_checknumber(L, 1)));
    lua_setfield(L, LUA_REGISTRYINDEX, OpacityKey);
    return 0;
}

static int GuiTransparency(lua_State* L)
{
    lua_pushnumber(L, std::max(0.0, (4.0 - luaL_checknumber(L, 1)) / 4.0));
    lua_setfield(L, LUA_REGISTRYINDEX, OpacityKey);
    return 0;
}

static int GuiParseColor(lua_State* L)
{
    u32 c = ColorArg(L, 1, 0);
    lua_pushinteger(L, c >> 24);
    lua_pushinteger(L, (c >> 16) & 0xFF);
    lua_pushinteger(L, (c >> 8) & 0xFF);
    lua_pushinteger(L, c & 0xFF);
    return 4;
}

static int GuiPopup(lua_State* L)
{
    QString message = QString::fromUtf8(luaL_tolstring(L, 1, nullptr));
    QString type = QString::fromUtf8(luaL_optstring(L, 2, "ok"));
    Print(L, "[popup] " + message);
    Instance(L)->osdAddMessage(0, "%s", message.toUtf8().constData());
    lua_pushstring(L, type.startsWith("yes") ? "yes" : "ok");
    return 1;
}

static int GuiSetLayerMask(lua_State* L)
{
    int main = (int)luaL_checkinteger(L, 1);
    int sub = (int)luaL_optinteger(L, 2, 0x1F);
    Frames(L).setHiddenLayers(0, ~main & 0x1F);
    Frames(L).setHiddenLayers(1, ~sub & 0x1F);
    return 0;
}

static QByteArray GdString(const QImage& image)
{
    QByteArray out;
    int w = image.width(), h = image.height();
    out.reserve(11 + w * h * 4);
    const char header[11] = {(char)0xFF, (char)0xFE, (char)(w >> 8), (char)w, (char)(h >> 8), (char)h, 1,
        (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF};
    out.append(header, 11);
    for (int y = 0; y < h; y++)
    {
        const QRgb* row = (const QRgb*)image.constScanLine(y);
        for (int x = 0; x < w; x++)
        {
            char px[4] = {0, (char)qRed(row[x]), (char)qGreen(row[x]), (char)qBlue(row[x])};
            out.append(px, 4);
        }
    }
    return out;
}

static int GuiGdScreenshot(lua_State* L)
{
    QString which = QString::fromUtf8(luaL_optstring(L, 1, "both")).toLower();
    QImage image;
    bool ok = false;
    Exec(L, [&](NDS& nds) { ok = FrameTools::grabScreens(nds, image); });
    if (!ok)
        return luaL_error(L, "the screen could not be read");
    if (which == "top")
        image = image.copy(0, 0, 256, 192);
    else if (which == "bottom")
        image = image.copy(0, 192, 256, 192);
    QByteArray gd = GdString(image);
    lua_pushlstring(L, gd.constData(), gd.size());
    return 1;
}

static int GuiGdOverlay(lua_State* L)
{
    int idx = 1;
    int dx = 0, dy = 0;
    if (lua_type(L, 1) == LUA_TNUMBER && lua_type(L, 2) == LUA_TNUMBER)
    {
        dx = (int)lua_tointeger(L, 1);
        dy = (int)lua_tointeger(L, 2);
        idx = 3;
    }
    size_t length = 0;
    const u8* data = (const u8*)luaL_checklstring(L, idx, &length);
    if (length < 11 || data[0] != 0xFF || data[1] != 0xFE)
        return luaL_argerror(L, idx, "expected a truecolor gd image string");
    int w = (data[2] << 8) | data[3], h = (data[4] << 8) | data[5];
    if (length < 11 + (size_t)w * h * 4)
        return luaL_argerror(L, idx, "gd image data is truncated");
    int sx = 0, sy = 0, sw = w, sh = h;
    double alpha = 1.0;
    int next = idx + 1;
    if (lua_type(L, next) == LUA_TNUMBER && lua_type(L, next + 3) == LUA_TNUMBER)
    {
        sx = (int)lua_tointeger(L, next);
        sy = (int)lua_tointeger(L, next + 1);
        sw = (int)lua_tointeger(L, next + 2);
        sh = (int)lua_tointeger(L, next + 3);
        next += 4;
    }
    if (lua_type(L, next) == LUA_TNUMBER)
        alpha = lua_tonumber(L, next);
    alpha *= Opacity(L);
    QImage image(w, h, QImage::Format_ARGB32);
    const u8* px = data + 11;
    for (int y = 0; y < h; y++)
    {
        QRgb* row = (QRgb*)image.scanLine(y);
        for (int x = 0; x < w; x++, px += 4)
        {
            int a = 255 - std::min(127, (int)px[0]) * 255 / 127;
            row[x] = qRgba(px[1], px[2], px[3], a);
        }
    }
    Frames(L).paintLua([&](QPainter& p) {
        p.setOpacity(std::clamp(alpha, 0.0, 1.0));
        p.drawImage(QPoint(dx, dy + 192), image, QRect(sx, sy, sw, sh));
    });
    return 0;
}

struct KeyName
{
    const char* Name;
    int Bit;
};

static const KeyName JoyKeys[] = {
    {"A", 0}, {"B", 1}, {"select", 2}, {"start", 3}, {"right", 4}, {"left", 5}, {"up", 6}, {"down", 7},
    {"R", 8}, {"L", 9}, {"X", 10}, {"Y", 11},
    {"Select", 2}, {"Start", 3}, {"Right", 4}, {"Left", 5}, {"Up", 6}, {"Down", 7},
};

static void PushJoy(lua_State* L, const FrameInput& in, bool reportUp, bool reportDown)
{
    lua_newtable(L);
    for (const KeyName& k : JoyKeys)
    {
        bool down = in.Keys & (1 << k.Bit);
        if ((down && reportDown) || (!down && reportUp))
        {
            lua_pushboolean(L, down);
            lua_setfield(L, -2, k.Name);
        }
    }
    if ((in.Lid && reportDown) || (!in.Lid && reportUp))
    {
        lua_pushboolean(L, in.Lid);
        lua_setfield(L, -2, "lid");
    }
    if (reportUp)
    {
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "debug");
    }
}

static int JoyGet(lua_State* L) { PushJoy(L, Frames(L).lastInput(), true, true); return 1; }
static int JoyGetDown(lua_State* L) { PushJoy(L, Frames(L).lastInput(), false, true); return 1; }
static int JoyGetUp(lua_State* L) { PushJoy(L, Frames(L).lastInput(), true, false); return 1; }
static int JoyPeek(lua_State* L) { PushJoy(L, Frames(L).userInput(), true, true); return 1; }
static int JoyPeekDown(lua_State* L) { PushJoy(L, Frames(L).userInput(), false, true); return 1; }
static int JoyPeekUp(lua_State* L) { PushJoy(L, Frames(L).userInput(), true, false); return 1; }

static int JoySet(lua_State* L)
{
    int idx = lua_type(L, 1) == LUA_TNUMBER ? 2 : 1;
    luaL_checktype(L, idx, LUA_TTABLE);
    InputPatch patch;
    for (const KeyName& k : JoyKeys)
    {
        if (lua_getfield(L, idx, k.Name) != LUA_TNIL)
        {
            if (lua_toboolean(L, -1))
                patch.Press |= 1 << k.Bit;
            else
                patch.Release |= 1 << k.Bit;
        }
        lua_pop(L, 1);
    }
    if (lua_getfield(L, idx, "lid") != LUA_TNIL)
        patch.Lid = lua_toboolean(L, -1);
    lua_pop(L, 1);
    Frames(L).patchInput(patch);
    return 0;
}

static void PushStylus(lua_State* L, const FrameInput& in)
{
    lua_newtable(L);
    lua_pushinteger(L, in.TouchX);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, in.TouchY);
    lua_setfield(L, -2, "y");
    lua_pushboolean(L, in.Touching);
    lua_setfield(L, -2, "touch");
}

static int StylusGet(lua_State* L) { PushStylus(L, Frames(L).lastInput()); return 1; }
static int StylusPeek(lua_State* L) { PushStylus(L, Frames(L).userInput()); return 1; }

static int StylusSet(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    FrameInput last = Frames(L).lastInput();
    InputPatch patch;
    bool hasXY = false;
    int x = last.TouchX, y = last.TouchY;
    if (lua_getfield(L, 1, "x") == LUA_TNUMBER)
    {
        x = (int)lua_tointeger(L, -1);
        hasXY = true;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "y") == LUA_TNUMBER)
    {
        y = (int)lua_tointeger(L, -1);
        hasXY = true;
    }
    lua_pop(L, 1);
    int touch = lua_getfield(L, 1, "touch");
    bool touching = touch == LUA_TNIL ? hasXY : lua_toboolean(L, -1);
    lua_pop(L, 1);
    patch.Touch = touching ? 1 : 0;
    patch.X = std::clamp(x, 0, 255);
    patch.Y = std::clamp(y, 0, 191);
    Frames(L).patchInput(patch);
    return 0;
}

static QString KeyNameOf(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QString(QChar('A' + (key - Qt::Key_A)));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QString(QChar('0' + (key - Qt::Key_0)));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return QString("F%1").arg(key - Qt::Key_F1 + 1);
    switch (key)
    {
    case Qt::Key_Backspace: return "backspace";
    case Qt::Key_Tab: return "tab";
    case Qt::Key_Return:
    case Qt::Key_Enter: return "enter";
    case Qt::Key_Shift: return "shift";
    case Qt::Key_Control: return "control";
    case Qt::Key_Alt: return "alt";
    case Qt::Key_Pause: return "pause";
    case Qt::Key_CapsLock: return "capslock";
    case Qt::Key_Escape: return "escape";
    case Qt::Key_Space: return "space";
    case Qt::Key_PageUp: return "pageup";
    case Qt::Key_PageDown: return "pagedown";
    case Qt::Key_End: return "end";
    case Qt::Key_Home: return "home";
    case Qt::Key_Left: return "left";
    case Qt::Key_Up: return "up";
    case Qt::Key_Right: return "right";
    case Qt::Key_Down: return "down";
    case Qt::Key_Insert: return "insert";
    case Qt::Key_Delete: return "delete";
    case Qt::Key_NumLock: return "numlock";
    case Qt::Key_ScrollLock: return "scrolllock";
    case Qt::Key_Semicolon: return "semicolon";
    case Qt::Key_Plus:
    case Qt::Key_Equal: return "plus";
    case Qt::Key_Comma: return "comma";
    case Qt::Key_Minus: return "minus";
    case Qt::Key_Period: return "period";
    case Qt::Key_Slash: return "slash";
    case Qt::Key_AsciiTilde:
    case Qt::Key_QuoteLeft: return "tilde";
    case Qt::Key_BracketLeft: return "leftbracket";
    case Qt::Key_Backslash: return "backslash";
    case Qt::Key_BracketRight: return "rightbracket";
    case Qt::Key_Apostrophe: return "quote";
    }
    return QString();
}

static int InputGet(lua_State* L)
{
    FrameTools::HostInput host = Frames(L).hostInput();
    lua_newtable(L);
    for (int key : host.Keys)
    {
        QString name = KeyNameOf(key & ~Qt::KeyboardModifierMask);
        if (name.isEmpty())
            continue;
        lua_pushboolean(L, true);
        lua_setfield(L, -2, name.toUtf8().constData());
    }
    if (host.Left) { lua_pushboolean(L, true); lua_setfield(L, -2, "leftclick"); }
    if (host.Right) { lua_pushboolean(L, true); lua_setfield(L, -2, "rightclick"); }
    if (host.Middle) { lua_pushboolean(L, true); lua_setfield(L, -2, "middleclick"); }
    lua_pushinteger(L, host.X);
    lua_setfield(L, -2, "xmouse");
    lua_pushinteger(L, host.Y);
    lua_setfield(L, -2, "ymouse");
    return 1;
}

void UpdateHostInput(EmuInstance* inst)
{
    FrameTools::HostInput host;
    host.Keys = inst->heldKeys;
    Qt::MouseButtons buttons = QGuiApplication::mouseButtons();
    host.Left = buttons & Qt::LeftButton;
    host.Right = buttons & Qt::RightButton;
    host.Middle = buttons & Qt::MiddleButton;
    if (MainWindow* win = inst->getMainWindow())
    {
        if (ScreenPanel* panel = win->panel)
        {
            QPoint pos = panel->mapFromGlobal(QCursor::pos());
            int x = pos.x(), y = pos.y();
            if (panel->mapToTouchScreen(x, y))
            {
                host.X = x;
                host.Y = y;
            }
        }
    }
    inst->getDebugBridge()->frames().setHostInput(host);
}

struct StateObject
{
    int Slot = 0;
    std::vector<u8> Data;
    bool HasMovie = false;
    MovieSnapshot Movie;
};

static StateObject* ToState(lua_State* L, int idx)
{
    auto box = (StateObject**)luaL_testudata(L, idx, StateMeta);
    return box ? *box : nullptr;
}

static int StateGC(lua_State* L)
{
    auto box = (StateObject**)luaL_checkudata(L, 1, StateMeta);
    delete *box;
    *box = nullptr;
    return 0;
}

static int StateCreate(lua_State* L)
{
    int slot = (int)luaL_optinteger(L, 1, 0);
    auto box = (StateObject**)lua_newuserdatauv(L, sizeof(StateObject*), 1);
    *box = new StateObject();
    (*box)->Slot = slot;
    luaL_setmetatable(L, StateMeta);
    return 1;
}

static std::string StatePath(lua_State* L, int idx, StateObject*& object)
{
    object = ToState(L, idx);
    if (object)
        return object->Slot ? Frames(L).slotPath(object->Slot) : std::string();
    if (lua_type(L, idx) == LUA_TNUMBER)
        return Frames(L).slotPath((int)lua_tointeger(L, idx));
    return luaL_checkstring(L, idx);
}

static void PushStateData(lua_State* L, int idx, StateObject* object, const std::string& path)
{
    if (object)
    {
        lua_getiuservalue(L, idx, 1);
        return;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, FileDataKey);
    if (lua_istable(L, -1))
        lua_getfield(L, -1, path.c_str());
    else
        lua_pushnil(L);
    lua_remove(L, -2);
}

static void StoreStateData(lua_State* L, int idx, StateObject* object, const std::string& path)
{
    if (object)
    {
        lua_setiuservalue(L, idx, 1);
        return;
    }
    if (lua_getfield(L, LUA_REGISTRYINDEX, FileDataKey) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, FileDataKey);
    }
    lua_insert(L, -2);
    lua_setfield(L, -2, path.c_str());
    lua_pop(L, 1);
}

static void SaveScriptData(lua_State* L, int idx, StateObject* object, const std::string& path)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, CallbackKeys[cb_Save]) != LUA_TFUNCTION)
    {
        lua_pop(L, 1);
        return;
    }
    int base = lua_gettop(L) - 1;
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK)
    {
        Print(L, QString("lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    int count = lua_gettop(L) - base;
    lua_createtable(L, count, 1);
    for (int i = 0; i < count; i++)
    {
        lua_pushvalue(L, base + 1 + i);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushinteger(L, count);
    lua_setfield(L, -2, "n");
    StoreStateData(L, idx, object, path);
    lua_settop(L, base);
}

static void LoadScriptData(lua_State* L, int idx, StateObject* object, const std::string& path)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, CallbackKeys[cb_Load]) != LUA_TFUNCTION)
    {
        lua_pop(L, 1);
        return;
    }
    PushStateData(L, idx, object, path);
    int count = 0;
    if (lua_istable(L, -1))
    {
        int data = lua_gettop(L);
        lua_getfield(L, data, "n");
        count = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        luaL_checkstack(L, count, "too many values");
        for (int i = 1; i <= count; i++)
            lua_rawgeti(L, data, i);
        lua_remove(L, data);
    }
    else
    {
        lua_pop(L, 1);
    }
    if (lua_pcall(L, count, 0, 0) != LUA_OK)
    {
        Print(L, QString("lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static bool CaptureState(lua_State* L, StateObject* object)
{
    NDS* nds = Instance(L)->getNDS();
    if (!nds)
        return false;
    Savestate state;
    if (state.Error)
        return false;
    nds->DoSavestate(&state);
    if (state.Error)
        return false;
    const u8* buffer = (const u8*)state.Buffer();
    object->Data.assign(buffer, buffer + state.Length());
    object->HasMovie = Frames(L).movie().snapshot(object->Movie);
    return true;
}

static bool RestoreState(lua_State* L, StateObject* object)
{
    NDS* nds = Instance(L)->getNDS();
    if (!nds || object->Data.empty())
        return false;
    std::vector<u8> copy = object->Data;
    Savestate state(copy.data(), copy.size(), false);
    if (!nds->DoSavestate(&state) || state.Error)
        return false;
    Frames(L).movie().restore(*nds, object->HasMovie ? &object->Movie : nullptr);
    return true;
}

static int StateOp(lua_State* L, bool save)
{
    StateObject* object = nullptr;
    std::string path = StatePath(L, 1, object);
    FrameTools& frames = Frames(L);
    if (!frames.atBoundary())
    {
        if (object && path.empty())
            return luaL_error(L, "savestate objects work in the script's main loop or in frame callbacks");
        if (Instance(L)->getDebugBridge()->onEmuThread())
            return luaL_error(L, "savestates cannot be saved or loaded inside a memory hook");
        frames.atNextFrame([&frames, path, save] {
            if (save)
                frames.saveStateFile(path);
            else
                frames.loadStateFile(path);
        });
        return 0;
    }
    bool ok;
    if (path.empty())
        ok = save ? CaptureState(L, object) : RestoreState(L, object);
    else
        ok = save ? frames.saveStateFile(path) : frames.loadStateFile(path);
    if (!ok)
        return luaL_error(L, save ? "savestate.save failed" : "savestate.load failed");
    if (save)
        SaveScriptData(L, 1, object, path);
    else
        LoadScriptData(L, 1, object, path);
    return 0;
}

static int StateSave(lua_State* L) { return StateOp(L, true); }
static int StateLoad(lua_State* L) { return StateOp(L, false); }

static int StateVerify(lua_State* L)
{
    lua_pushboolean(L, true);
    return 1;
}

static int StateSaveScriptData(lua_State* L)
{
    StateObject* object = nullptr;
    std::string path = StatePath(L, 1, object);
    SaveScriptData(L, 1, object, path);
    return 0;
}

static int StateLoadScriptData(lua_State* L)
{
    StateObject* object = nullptr;
    std::string path = StatePath(L, 1, object);
    PushStateData(L, 1, object, path);
    if (!lua_istable(L, -1))
        return 0;
    int data = lua_gettop(L);
    lua_getfield(L, data, "n");
    int count = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    luaL_checkstack(L, count, "too many values");
    for (int i = 1; i <= count; i++)
        lua_rawgeti(L, data, i);
    return count;
}

static int MovieActive(lua_State* L)
{
    lua_pushboolean(L, Frames(L).movie().status().State != Movie::Inactive);
    return 1;
}

static int MovieRecording(lua_State* L)
{
    lua_pushboolean(L, Frames(L).movie().status().State == Movie::Recording);
    return 1;
}

static int MoviePlaying(lua_State* L)
{
    lua_pushboolean(L, Frames(L).movie().status().State == Movie::Playing);
    return 1;
}

static int MovieMode(lua_State* L)
{
    Movie::Mode s = Frames(L).movie().status().State;
    if (s == Movie::Recording)
        lua_pushstring(L, "record");
    else if (s == Movie::Playing)
        lua_pushstring(L, "playback");
    else
        lua_pushnil(L);
    return 1;
}

static int MovieLength(lua_State* L)
{
    lua_pushinteger(L, Frames(L).movie().status().Length);
    return 1;
}

static int MovieName(lua_State* L)
{
    lua_pushstring(L, Frames(L).movie().status().Path.toUtf8().constData());
    return 1;
}

static int MovieRerecords(lua_State* L)
{
    lua_pushinteger(L, Frames(L).movie().status().Rerecords);
    return 1;
}

static int MovieSetRerecords(lua_State* L)
{
    Frames(L).movie().setRerecords((u32)luaL_checkinteger(L, 1));
    return 0;
}

static int MovieReadOnly(lua_State* L)
{
    lua_pushboolean(L, Frames(L).movie().status().ReadOnly);
    return 1;
}

static int MovieSetReadOnly(lua_State* L)
{
    Frames(L).movie().setReadOnly(lua_toboolean(L, 1));
    return 0;
}

static int MovieFrameCount(lua_State* L)
{
    Movie::Status s = Frames(L).movie().status();
    lua_pushinteger(L, s.State == Movie::Inactive ? Frames(L).frameCount() : s.Frame);
    return 1;
}

static int MoviePlay(lua_State* L)
{
    QString path = QString::fromUtf8(luaL_checkstring(L, 1));
    bool readOnly = lua_isnoneornil(L, 2) ? Frames(L).movie().status().ReadOnly : lua_toboolean(L, 2);
    Frames(L).movie().requestPlay(path, readOnly);
    return 0;
}

static int MovieStop(lua_State* L)
{
    Frames(L).movie().requestStop();
    return 0;
}

static void SetFuncs(lua_State* L, const char* table, const luaL_Reg* funcs)
{
    if (lua_getglobal(L, table) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, table);
    }
    luaL_setfuncs(L, funcs, 0);
    lua_pop(L, 1);
}

static const char* const Prelude = R"lua(
local N = ...

local function cpuname(c)
  if c == nil then return 9 end
  if type(c) == "number" then return (c == 7 or c == 1) and 7 or 9 end
  c = tostring(c):lower()
  if c:find("arm7") or c == "sub" then return 7 end
  return 9
end

local original = { readbyte = memory.readbyte, writebyte = memory.writebyte }
local function reader(size, signed)
  return function(a, c) return N.read(a, size, signed, cpuname(c)) end
end
local function writer(size)
  return function(a, v, c) N.write(a, size, v, cpuname(c)) end
end

memory.readbyte = function(a, ...)
  if a < 0x01000000 and original.readbyte then return original.readbyte(a, ...) end
  return N.read(a, 1, false, cpuname((...)))
end
memory.writebyte = function(a, v, ...)
  if a < 0x01000000 and original.writebyte then return original.writebyte(a, v, ...) end
  N.write(a, 1, v, cpuname((...)))
end
memory.readbytesigned = reader(1, true)
memory.readbyteunsigned = reader(1, false)
memory.readword = reader(2, false)
memory.readwordsigned = reader(2, true)
memory.readwordunsigned = reader(2, false)
memory.readshort = reader(2, false)
memory.readshortsigned = reader(2, true)
memory.readshortunsigned = reader(2, false)
memory.readdword = reader(4, false)
memory.readdwordsigned = reader(4, true)
memory.readdwordunsigned = reader(4, false)
memory.readlong = reader(4, false)
memory.readlongsigned = reader(4, true)
memory.readlongunsigned = reader(4, false)
memory.writeword = writer(2)
memory.writeshort = writer(2)
memory.writedword = writer(4)
memory.writelong = writer(4)
memory.readbyterange = function(a, n, c) return N.range(a, n, cpuname(c)) end
memory.vram_readword = function(a) return N.read(0x06000000 | (a & 0xFFFFFF), 2, false, 9) end
memory.vram_writeword = function(a, v) N.write(0x06000000 | (a & 0xFFFFFF), 2, v, 9) end
memory.isvalid = function(a)
  local top = (a >> 24) & 0xFF
  return (top >= 0x01 and top <= 0x0A) or top == 0xFF
end

local function splitreg(name)
  local n = tostring(name):lower()
  local cpu = 9
  local prefix = n:match("^(%w+)%.")
  if prefix then
    if prefix == "arm7" or prefix == "sub" then cpu = 7 end
    n = n:sub(#prefix + 2)
  end
  return cpu, n
end
memory.getregister = function(name) local c, r = splitreg(name) return N.reg(c, r) end
memory.setregister = function(name, v) local c, r = splitreg(name) N.setreg(c, r, v) end

local function hook(kind, defsize)
  return function(addr, a, b, c)
    local size, cpu, fn = defsize, 9, nil
    for _, v in ipairs({a, b, c}) do
      if type(v) == "number" then size = v
      elseif type(v) == "string" then cpu = cpuname(v)
      elseif type(v) == "function" then fn = v end
    end
    if size < 0 then size = -size; addr = addr - size end
    local name = "desmume." .. kind .. "." .. cpu .. "." .. addr
    event.unregisterbyname(name)
    if fn then
      if kind == "exec" then
        event.onmemoryexecute(function(at) fn(at, size) end, addr, name, cpu)
      elseif kind == "read" then
        event.onmemoryread(function(at) fn(at, size) end, addr, name, cpu, size)
      else
        event.onmemorywrite(function(at) fn(at, size) end, addr, name, cpu, size)
      end
    end
  end
end
memory.registerwrite = hook("write", 1)
memory.register = memory.registerwrite
memory.registerread = hook("read", 1)
memory.registerexec = hook("exec", 2)
memory.registerrun = memory.registerexec
memory.registerexecute = memory.registerexec

emu.emulateframefastnoskipping = emu.emulateframe
emu.emulateframefast = emu.emulateframe
emu.emulateframeinvisible = emu.emulateframe
emu.speedmode = function() end
emu.wait = function() end
emu.redraw = function() end
emu.print = function(...) print(...) end
emu.persistglobalvariables = function(t)
  for k, v in pairs(t or {}) do if _G[k] == nil then _G[k] = v end end
end
local function unsupported(name) return function() error(name .. " is not supported in melonDS") end end
emu.openscript = unsupported("emu.openscript")
emu.loadrom = unsupported("emu.loadrom")
emu.openrom = unsupported("emu.openrom")
emu.addmenu = function() end
emu.setmenuiteminfo = function() end
emu.registermenustart = function() end
emu.register3devent = function() end
emu.set3dtransform = function() end

gui.osdtext = gui.text
gui.drawtext = gui.text
gui.drawbox = gui.box
gui.rect = gui.box
gui.drawrect = gui.box
gui.drawline = gui.line
gui.drawpixel = gui.pixel
gui.setpixel = gui.pixel
gui.writepixel = gui.pixel
gui.readpixel = gui.getpixel
gui.drawimage = gui.gdoverlay
gui.image = gui.gdoverlay

joypad.read = joypad.get
joypad.readdown = joypad.getdown
joypad.readup = joypad.getup
joypad.write = joypad.set
stylus.read = stylus.get
stylus.write = stylus.set
input.read = input.get
input.registerhotkey = function() end
input.popup = gui.popup

movie.getname = movie.name
movie.getreadonly = movie.readonly
movie.open = movie.play
movie.playback = movie.play
movie.replay = movie.play
movie.close = movie.stop
movie.rerecordcounting = function() end

sound.clear = function() end
controller.get = function() return {} end
controller.read = controller.get

local function int(x) return math.floor(tonumber(x) or 0) end
local function tobit(x)
  x = int(x) & 0xFFFFFFFF
  if x >= 0x80000000 then x = x - 0x100000000 end
  return x
end
bit.tobit = tobit
bit.band = function(a, ...) local r = int(a) for _, v in ipairs({...}) do r = r & int(v) end return tobit(r) end
bit.bor = function(a, ...) local r = int(a) for _, v in ipairs({...}) do r = r | int(v) end return tobit(r) end
bit.bxor = function(a, ...) local r = int(a) for _, v in ipairs({...}) do r = r ~ int(v) end return tobit(r) end
bit.bnot = function(a) return tobit(~int(a)) end
bit.lshift = function(a, n) return tobit(int(a) << (int(n) & 31)) end
bit.rshift = function(a, n) return tobit((int(a) & 0xFFFFFFFF) >> (int(n) & 31)) end
bit.arshift = function(a, n) return tobit(tobit(a) >> (int(n) & 31) | (tobit(a) < 0 and ~(0xFFFFFFFF >> (int(n) & 31)) or 0)) end
bit.rol = function(a, n) a = int(a) & 0xFFFFFFFF n = int(n) & 31 return tobit((a << n) | (a >> (32 - n))) end
bit.ror = function(a, n) a = int(a) & 0xFFFFFFFF n = int(n) & 31 return tobit((a >> n) | (a << (32 - n))) end
bit.bswap = function(a) a = int(a) & 0xFFFFFFFF return tobit(((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24)) end
bit.tohex = function(a, n)
  n = n or 8
  local fmt = n < 0 and "%0" .. -n .. "X" or "%0" .. n .. "x"
  local s = string.format(fmt, int(a) & 0xFFFFFFFF)
  return s:sub(-math.abs(n))
end
AND = function(...) return bit.band(...) end
OR = function(...) return bit.bor(...) end
XOR = function(...) return bit.bxor(...) end
SHIFT = function(a, n) if n >= 0 then return bit.rshift(a, n) end return bit.lshift(a, -n) end
BIT = function(n) return 1 << n end

local function describe(v, depth)
  if type(v) == "table" and depth < 3 then
    local parts = {}
    for k, x in pairs(v) do parts[#parts + 1] = tostring(k) .. "=" .. describe(x, depth + 1) end
    return "{" .. table.concat(parts, ", ") .. "}"
  end
  return tostring(v)
end
print = function(...)
  local out = {}
  for i = 1, select("#", ...) do out[i] = describe((select(i, ...)), 0) end
  N.print(table.concat(out, " "))
end
copytable = function(t)
  if type(t) ~= "table" then return t end
  local c = {}
  for k, v in pairs(t) do c[k] = v end
  return setmetatable(c, getmetatable(t))
end
addressof = function(v) return tonumber(tostring(v):match("0x%x+") or "0") end

unpack = unpack or table.unpack
loadstring = loadstring or load
math.pow = math.pow or function(a, b) return a ^ b end
math.mod = math.mod or math.fmod
table.getn = table.getn or function(t) return #t end
string.gfind = string.gfind or string.gmatch
)lua";

void Register(lua_State* L)
{
    static const luaL_Reg emuFuncs[] = {
        {"frameadvance", EmuFrameAdvance},
        {"framecount", EmuFrameCount},
        {"lagcount", EmuLagCount},
        {"lagged", EmuLagged},
        {"emulating", EmuEmulating},
        {"atframeboundary", EmuAtFrameBoundary},
        {"registerbefore", EmuRegisterBefore},
        {"registerafter", EmuRegisterAfter},
        {"registerstart", EmuRegisterStart},
        {"registerexit", EmuRegisterExit},
        {"pause", EmuPause},
        {"unpause", EmuUnpause},
        {"reset", EmuReset},
        {"message", EmuMessage},
        {"gamecode", EmuGameCode4},
        {"smallgamecode", EmuGameCode3},
        {"emulateframe", EmuEmulateFrame},
        {nullptr, nullptr},
    };
    static const luaL_Reg guiFuncs[] = {
        {"register", GuiRegister},
        {"text", GuiText},
        {"box", GuiBox},
        {"line", GuiLine},
        {"pixel", GuiPixel},
        {"getpixel", GuiGetPixel},
        {"opacity", GuiOpacity},
        {"transparency", GuiTransparency},
        {"popup", GuiPopup},
        {"parsecolor", GuiParseColor},
        {"setlayermask", GuiSetLayerMask},
        {"gdscreenshot", GuiGdScreenshot},
        {"gdoverlay", GuiGdOverlay},
        {"redraw", EmuNothing},
        {nullptr, nullptr},
    };
    static const luaL_Reg joyFuncs[] = {
        {"get", JoyGet},
        {"getdown", JoyGetDown},
        {"getup", JoyGetUp},
        {"peek", JoyPeek},
        {"peekdown", JoyPeekDown},
        {"peekup", JoyPeekUp},
        {"set", JoySet},
        {nullptr, nullptr},
    };
    static const luaL_Reg stylusFuncs[] = {
        {"get", StylusGet},
        {"peek", StylusPeek},
        {"set", StylusSet},
        {nullptr, nullptr},
    };
    static const luaL_Reg inputFuncs[] = {
        {"get", InputGet},
        {nullptr, nullptr},
    };
    static const luaL_Reg stateFuncs[] = {
        {"create", StateCreate},
        {"save", StateSave},
        {"load", StateLoad},
        {"verify", StateVerify},
        {"savescriptdata", StateSaveScriptData},
        {"loadscriptdata", StateLoadScriptData},
        {"registersave", StateRegisterSave},
        {"registerload", StateRegisterLoad},
        {nullptr, nullptr},
    };
    static const luaL_Reg movieFuncs[] = {
        {"active", MovieActive},
        {"recording", MovieRecording},
        {"playing", MoviePlaying},
        {"mode", MovieMode},
        {"length", MovieLength},
        {"name", MovieName},
        {"rerecordcount", MovieRerecords},
        {"setrerecordcount", MovieSetRerecords},
        {"readonly", MovieReadOnly},
        {"setreadonly", MovieSetReadOnly},
        {"framecount", MovieFrameCount},
        {"play", MoviePlay},
        {"stop", MovieStop},
        {nullptr, nullptr},
    };
    static const luaL_Reg nativeFuncs[] = {
        {"read", NativeRead},
        {"write", NativeWrite},
        {"range", NativeRange},
        {"reg", NativeGetRegister},
        {"setreg", NativeSetRegister},
        {"print", NativePrint},
        {nullptr, nullptr},
    };

    SetFuncs(L, "emu", emuFuncs);
    SetFuncs(L, "gui", guiFuncs);
    SetFuncs(L, "joypad", joyFuncs);
    SetFuncs(L, "stylus", stylusFuncs);
    SetFuncs(L, "input", inputFuncs);
    SetFuncs(L, "savestate", stateFuncs);
    SetFuncs(L, "movie", movieFuncs);
    for (const char* table : {"memory", "sound", "controller", "bit", "event"})
    {
        static const luaL_Reg none[] = {{nullptr, nullptr}};
        SetFuncs(L, table, none);
    }

    luaL_newmetatable(L, StateMeta);
    lua_pushcfunction(L, StateGC);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    if (luaL_loadbuffer(L, Prelude, strlen(Prelude), "=desmume") != LUA_OK)
    {
        Print(L, QString("lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    luaL_newlib(L, nativeFuncs);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK)
    {
        Print(L, QString("lua: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

bool Start(lua_State* L, const char* file, QString& error)
{
    if (luaL_loadfile(L, file) != LUA_OK)
    {
        error = QString::fromUtf8(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    lua_State* co = lua_newthread(L);
    lua_pushvalue(L, -2);
    lua_xmove(L, co, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, MainKey);
    lua_pop(L, 1);
    return Resume(L, error) >= 0;
}

void Frame(lua_State* L, bool after)
{
    if (get_bundle(L)->flagPause || get_bundle(L)->flagStop)
        return;
    if (!after)
    {
        RunCallback(L, cb_Before);
        return;
    }
    RunCallback(L, cb_After);
    RunCallback(L, cb_Gui);
    QString error;
    if (Resume(L, error) < 0)
        Print(L, error);
}

void Close(lua_State* L)
{
    RunCallback(L, cb_Exit);
    for (int i = 0; i < cb_Count; i++)
    {
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, CallbackKeys[i]);
    }
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, MainKey);
}

}
