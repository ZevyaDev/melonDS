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

#include <QStringDecoder>
#include <QStringList>

#include <algorithm>
#include <cstring>

#include "CallArgs.h"
#include "DebugSymbols.h"
#include "DebugTool.h"
#include "NDS.h"

using namespace melonDS;

namespace CallArgs
{

namespace
{

constexpr int RegisterSlots = 4;
constexpr int StackSlots = 4;
constexpr int MaxString = 256;
constexpr int ShortString = 40;
constexpr int MinGuessedString = 3;
constexpr u64 MaxDecimal = 0xFFFF;

enum class Kind
{
    Signed,
    Unsigned,
    Bool,
    Float,
    Double,
    Pointer,
    String,
    MaybeString,
    Varargs,
};

struct Param
{
    QString Type;
    Kind K;
    int Bits;
    bool This = false;
};

struct Value
{
    QString Short;
    QString Long;
};

bool IsDataPointer(u32 v)
{
    u32 region = v >> 24;
    return region == 0x02 || region == 0x03;
}

bool IsTextByte(u8 b)
{
    return (b >= 0x20 && b < 0x7F) || b == '\t' || b == '\n' || b == '\r';
}

QByteArray ReadString(NDS& nds, int cpu, u32 addr)
{
    QByteArray out;
    for (int i = 0; i < MaxString; i++)
    {
        u8 b = (u8)nds.Debugger.Peek(cpu, addr + i, 1);
        if (b == 0)
        {
            out.append('\0');
            break;
        }
        if (b < 0x20 && !IsTextByte(b))
            break;
        out.append((char)b);
    }
    return out;
}

bool IsPlainText(const QByteArray& raw)
{
    if (!raw.endsWith('\0') || raw.size() - 1 < MinGuessedString)
        return false;
    return std::all_of(raw.begin(), raw.end() - 1, [](char c) { return IsTextByte((u8)c); });
}

QString Quote(const QByteArray& raw, int limit)
{
    bool terminated = raw.endsWith('\0');
    QByteArray bytes = terminated ? raw.chopped(1) : raw;
    QString decoded;
    bool ascii = std::all_of(bytes.begin(), bytes.end(), [](char c) { return (u8)c < 0x80; });
    if (!ascii)
    {
        QStringDecoder sjis("Shift_JIS");
        if (sjis.isValid())
        {
            QString s = sjis(bytes);
            if (!sjis.hasError())
                decoded = s;
        }
    }
    QString body;
    auto add = [&body](char32_t c) {
        switch (c)
        {
        case '"': body += "\\\""; break;
        case '\\': body += "\\\\"; break;
        case '\n': body += "\\n"; break;
        case '\t': body += "\\t"; break;
        case '\r': body += "\\r"; break;
        default:
            if (c < 0x20 || (c >= 0x7F && c < 0x100))
                body += QString("\\x%1").arg((uint)c, 2, 16, QChar('0'));
            else
                body += QString::fromUcs4(&c, 1);
        }
    };
    if (!decoded.isEmpty())
        for (char32_t c : decoded.toUcs4())
            add(c);
    else
        for (char c : bytes)
            add((u8)c);
    bool more = !terminated;
    if (body.size() > limit)
    {
        body.truncate(limit);
        more = true;
    }
    return "\"" + body + "\"" + (more ? "…" : "");
}

QString Number(u64 v, Kind kind, int bits)
{
    switch (kind)
    {
    case Kind::Bool:
        return (v & 0xFF) ? "true" : "false";
    case Kind::Signed:
        return QString::number((s64)(v << (64 - bits)) >> (64 - bits));
    case Kind::Float:
    {
        float f;
        u32 w = (u32)v;
        memcpy(&f, &w, sizeof(f));
        return QString::number(f, 'g', 7);
    }
    case Kind::Double:
    {
        double d;
        memcpy(&d, &v, sizeof(d));
        return QString::number(d, 'g', 15);
    }
    default:
        if (bits < 64)
            v &= (1ull << bits) - 1;
        return v <= MaxDecimal ? QString::number(v) : "0x" + QString::number(v, 16).toUpper();
    }
}

QStringList SplitParams(const QString& list)
{
    QStringList out;
    int depth = 0, start = 0;
    for (int i = 0; i < list.size(); i++)
    {
        QChar c = list[i];
        if (c == '(' || c == '<' || c == '[')
            depth++;
        else if (c == ')' || c == '>' || c == ']')
            depth--;
        else if (c == ',' && depth == 0)
        {
            out << list.mid(start, i - start).trimmed();
            start = i + 1;
        }
    }
    QString last = list.mid(start).trimmed();
    if (!last.isEmpty())
        out << last;
    return out;
}

bool ParamList(QString pretty, QString& list)
{
    pretty = pretty.trimmed();
    for (;;)
    {
        if (pretty.endsWith(" const"))
            pretty.chop(6);
        else if (pretty.endsWith(" volatile"))
            pretty.chop(9);
        else if (pretty.endsWith('&'))
            pretty.chop(1);
        else
            break;
        pretty = pretty.trimmed();
    }
    if (!pretty.endsWith(')'))
        return false;
    int depth = 0;
    for (int i = pretty.size() - 1; i >= 0; i--)
    {
        QChar c = pretty[i];
        if (c == ')' || c == '>' || c == ']')
            depth++;
        else if (c == '(' || c == '<' || c == '[')
            depth--;
        if (depth == 0)
        {
            list = pretty.mid(i + 1, pretty.size() - i - 2);
            return true;
        }
    }
    return false;
}

Param Classify(const QString& raw)
{
    static const QHash<QString, std::pair<Kind, int>> scalars = {
        {"bool", {Kind::Bool, 8}},
        {"char", {Kind::Signed, 8}},
        {"signed char", {Kind::Signed, 8}},
        {"unsigned char", {Kind::Unsigned, 8}},
        {"short", {Kind::Signed, 16}},
        {"unsigned short", {Kind::Unsigned, 16}},
        {"wchar_t", {Kind::Unsigned, 16}},
        {"int", {Kind::Signed, 32}},
        {"long", {Kind::Signed, 32}},
        {"unsigned int", {Kind::Unsigned, 32}},
        {"unsigned long", {Kind::Unsigned, 32}},
        {"long long", {Kind::Signed, 64}},
        {"unsigned long long", {Kind::Unsigned, 64}},
        {"float", {Kind::Float, 32}},
        {"double", {Kind::Double, 64}},
        {"long double", {Kind::Double, 64}},
    };
    QString t = raw.simplified();
    if (t == "...")
        return {t, Kind::Varargs, 0};
    if (t.contains("::*)"))
        return {t, Kind::Unsigned, 64};
    if (t == "char const*" || t == "const char*")
        return {t, Kind::String, 32};
    if (t == "char*")
        return {t, Kind::MaybeString, 32};
    if (t.endsWith('*') || t.endsWith('&') || t.contains("(*)") || t.contains("(&)"))
        return {t, Kind::Pointer, 32};
    auto it = scalars.find(t);
    if (it != scalars.end())
        return {t, it->first, it->second};
    return {t, Kind::Unsigned, 32};
}

bool Signature(const DebugSymbols::Symbol& sym, QList<Param>& params)
{
    QString list;
    if (!sym.Name.startsWith("_Z") || sym.Pretty == sym.Name || !ParamList(sym.Pretty, list))
        return false;
    if (sym.Name.startsWith("_ZN"))
        params << Param{"this", Kind::Pointer, 32, true};
    for (const QString& p : SplitParams(list))
        if (p != "void")
            params << Classify(p);
    return true;
}

Value Guess(u32 v, const Memory& mem)
{
    auto it = mem.Strings.find(v);
    if (IsDataPointer(v) && it != mem.Strings.end() && IsPlainText(*it))
        return {Quote(*it, ShortString), Hex(v) + "  " + Quote(*it, MaxString)};
    if (IsDataPointer(v))
        return {Hex(v), Hex(v)};
    QString n = Number(v, Kind::Unsigned, 32);
    return {n, v > 9 && v <= MaxDecimal ? n + " (0x" + QString::number(v, 16).toUpper() + ")" : n};
}

Value Format(const Param& p, u64 v, const DebugSymbols& syms, const Memory& mem)
{
    u32 lo = (u32)v;
    if (p.K == Kind::String || p.K == Kind::MaybeString)
    {
        auto it = mem.Strings.find(lo);
        if (IsDataPointer(lo) && it != mem.Strings.end() && !it->isEmpty() && (p.K == Kind::String || IsPlainText(*it)))
            return {Quote(*it, ShortString), Hex(lo) + "  " + Quote(*it, MaxString)};
    }
    if (p.K == Kind::Pointer || p.K == Kind::String || p.K == Kind::MaybeString)
    {
        if (!lo)
            return {"NULL", "NULL"};
        QString exact = syms.exact(lo);
        QString where = syms.describe(lo);
        return {exact.isEmpty() ? Hex(lo) : "&" + exact, where.isEmpty() ? Hex(lo) : Hex(lo) + "  " + where};
    }
    u64 masked = p.Bits >= 64 ? v : v & ((1ull << p.Bits) - 1);
    QString n = Number(v, p.K, p.Bits);
    bool integer = p.K == Kind::Signed || p.K == Kind::Unsigned;
    if (!integer || n.startsWith("0x") || masked <= 9)
        return {n, n};
    return {n, n + " (0x" + QString::number(masked, 16).toUpper() + ")"};
}

QString Row(const QString& where, const QString& type, const QString& value)
{
    return "<tr><td>" + where.toHtmlEscaped() + "</td><td>" + type.toHtmlEscaped() + "</td><td>"
        + value.toHtmlEscaped() + "</td></tr>";
}

}

void Collect(NDS& nds, int cpu, const std::vector<CallFrame>& frames, Memory& mem)
{
    for (const CallFrame& f : frames)
    {
        if (!f.HasArgs)
            continue;
        std::vector<u32> values(f.Args, f.Args + RegisterSlots);
        for (int k = 0; k < StackSlots; k++)
        {
            u32 addr = f.SP + 4 * k;
            u32 v = nds.Debugger.Peek(cpu, addr, 4);
            mem.Words.insert(addr, v);
            values.push_back(v);
        }
        for (u32 v : values)
            if (IsDataPointer(v) && !mem.Strings.contains(v))
                mem.Strings.insert(v, ReadString(nds, cpu, v));
    }
}

Shown Describe(const CallFrame& f, const DebugSymbols& syms, const Memory& mem)
{
    Shown out;
    if (!f.HasArgs)
        return out;
    const DebugSymbols::Symbol* sym = syms.lookup(f.Entry);
    if (sym && sym->Addr != (f.Entry & ~1u))
        sym = nullptr;
    QList<Param> params;
    bool typed = sym && Signature(*sym, params);

    auto slot = [&](int i, u32& v) {
        if (i < RegisterSlots)
        {
            v = f.Args[i];
            return true;
        }
        auto it = mem.Words.find(f.SP + 4 * (i - RegisterSlots));
        if (it == mem.Words.end())
            return false;
        v = *it;
        return true;
    };
    auto where = [](int i) {
        return i < RegisterSlots ? QString("r%1").arg(i) : QString("[sp+%1]").arg(4 * (i - RegisterSlots));
    };

    QStringList texts;
    QString rows;
    auto guessed = [&](int i, const QString& type) {
        Value val = Guess(f.Args[i], mem);
        texts << (typed ? val.Short : where(i) + "=" + val.Short);
        rows += Row(where(i), type, val.Long);
        if (IsDataPointer(f.Args[i]))
            out.Pointers << Pointer{where(i), f.Args[i]};
    };

    if (!typed)
    {
        for (int i = 0; i < RegisterSlots; i++)
            guessed(i, QString());
    }
    else
    {
        int next = 0;
        for (const Param& p : params)
        {
            if (p.K == Kind::Varargs)
            {
                for (; next < RegisterSlots; next++)
                    guessed(next, "...");
                break;
            }
            int words = p.Bits > 32 ? 2 : 1;
            u32 lo, hi = 0;
            if (!slot(next, lo) || (words == 2 && !slot(next + 1, hi)))
            {
                texts << "…";
                break;
            }
            Value val = Format(p, lo | ((u64)hi << 32), syms, mem);
            texts << (p.This ? "this=" + val.Short : val.Short);
            rows += Row(words == 2 ? where(next) + ", " + where(next + 1) : where(next), p.Type, val.Long);
            bool pointer = p.K == Kind::Pointer || p.K == Kind::String || p.K == Kind::MaybeString;
            if (pointer && IsDataPointer(lo))
                out.Pointers << Pointer{where(next) + " " + (p.This ? "this" : p.Type), lo};
            next += words;
        }
    }

    out.Text = texts.join(", ");
    QString title = sym ? syms.name(*sym) : Hex(f.Entry);
    QString note = typed ? "Arguments at the call" : "No C++ signature: r0-r3 at the call";
    out.Detail = "<b>" + title.toHtmlEscaped() + "</b><br>" + note + "<table>" + rows + "</table>";
    return out;
}

}
