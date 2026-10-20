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

#ifndef NITROOVERRIDE_H
#define NITROOVERRIDE_H

#include <vector>

#include <QString>
#include <QStringList>

#include "types.h"

namespace NitroOverride
{

bool Apply(std::vector<melonDS::u8>& rom, const QString& dir, QStringList& replaced, QString& error);

}

#endif
