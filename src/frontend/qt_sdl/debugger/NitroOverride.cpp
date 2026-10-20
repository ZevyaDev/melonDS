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

#include <QDir>
#include <QFile>

#include "NitroOverride.h"

using namespace melonDS;

namespace NitroOverride
{

constexpr u32 Alignment = 0x200;
constexpr u32 MaxROM = 512 * 1024 * 1024;

static u32 Read32(const std::vector<u8>& rom, u32 offset)
{
    if (offset + 4 > rom.size())
        return 0;
    u32 value;
    memcpy(&value, &rom[offset], 4);
    return value;
}

static u16 Read16(const std::vector<u8>& rom, u32 offset)
{
    if (offset + 2 > rom.size())
        return 0;
    u16 value;
    memcpy(&value, &rom[offset], 2);
    return value;
}

static void Write32(std::vector<u8>& rom, u32 offset, u32 value)
{
    memcpy(&rom[offset], &value, 4);
}

static u16 HeaderCRC(const u8* data, u32 length)
{
    u16 crc = 0xFFFF;
    for (u32 i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void WalkFNT(const std::vector<u8>& rom, u32 fnt, u32 fntSize, u16 dirID, const QString& prefix,
    std::vector<std::pair<u16, QString>>& files, int depth)
{
    if (depth > 64)
        return;
    u32 entry = fnt + (dirID & 0xFFF) * 8;
    if (entry + 8 > fnt + fntSize)
        return;
    u32 pos = fnt + Read32(rom, entry);
    u16 fileID = Read16(rom, entry + 4);
    while (pos < fnt + fntSize && pos < rom.size())
    {
        u8 type = rom[pos++];
        if (!type)
            break;
        u32 length = type & 0x7F;
        if (pos + length > rom.size())
            break;
        QString name = QString::fromLatin1((const char*)&rom[pos], length);
        pos += length;
        if (type & 0x80)
        {
            u16 sub = Read16(rom, pos);
            pos += 2;
            WalkFNT(rom, fnt, fntSize, sub, prefix + name + "/", files, depth + 1);
        }
        else
        {
            files.push_back({fileID++, prefix + name});
        }
    }
}

bool Apply(std::vector<u8>& rom, const QString& dir, QStringList& replaced, QString& error)
{
    if (rom.size() < 0x200)
    {
        error = "no cartridge ROM";
        return false;
    }
    u32 fnt = Read32(rom, 0x40), fntSize = Read32(rom, 0x44);
    u32 fat = Read32(rom, 0x48), fatSize = Read32(rom, 0x4C);
    if (!fntSize || !fatSize || fnt + fntSize > rom.size() || fat + fatSize > rom.size())
    {
        error = "this ROM has no NitroFS";
        return false;
    }
    std::vector<std::pair<u16, QString>> files;
    WalkFNT(rom, fnt, fntSize, 0xF000, QString(), files, 0);

    u32 used = Read32(rom, 0x80);
    for (u32 i = 0; i + 8 <= fatSize; i += 8)
        used = std::max(used, Read32(rom, fat + i + 4));
    u32 next = (used + Alignment - 1) & ~(Alignment - 1);

    QDir base(dir);
    QDir data(base.filePath("data"));
    for (const auto& [id, path] : files)
    {
        QString local = base.filePath(path);
        if (!QFile::exists(local) && data.exists())
            local = data.filePath(path);
        if (!QFile::exists(local))
            continue;
        QFile file(local);
        if (!file.open(QIODevice::ReadOnly))
        {
            error = "cannot read " + local;
            return false;
        }
        QByteArray bytes = file.readAll();
        u32 entry = fat + id * 8;
        if (entry + 8 > fat + fatSize)
            continue;
        u32 start = Read32(rom, entry), end = Read32(rom, entry + 4);
        u32 size = bytes.size();
        if (size > end - start)
        {
            start = next;
            if ((u64)start + size > MaxROM)
            {
                error = "the replaced files do not fit in 512 MB";
                return false;
            }
            next = (start + size + Alignment - 1) & ~(Alignment - 1);
            if (next > rom.size())
                rom.resize(next, 0xFF);
        }
        memcpy(&rom[start], bytes.constData(), size);
        Write32(rom, entry, start);
        Write32(rom, entry + 4, start + size);
        replaced << path;
    }
    if (replaced.isEmpty())
    {
        error = "no file in " + dir + " matches a NitroFS path";
        return false;
    }

    u32 newUsed = std::max(Read32(rom, 0x80), std::min<u32>(next, rom.size()));
    Write32(rom, 0x80, newUsed);
    u8 capacity = rom[0x14];
    while (capacity < 15 && (0x20000u << capacity) < rom.size())
        capacity++;
    rom[0x14] = capacity;
    u16 crc = HeaderCRC(rom.data(), 0x15E);
    memcpy(&rom[0x15E], &crc, 2);
    return true;
}

}
