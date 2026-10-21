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

#include "RomOverlays.h"
#include "../NDS.h"
#include "../NDSCart.h"

namespace melonDS
{

std::vector<RomOverlay> ReadRomOverlays(const NDS& nds)
{
    const NDSCart::CartCommon* cart = nds.GetNDSCart();
    if (!cart || !cart->GetROM())
        return {};
    return ReadRomOverlays(cart->GetROM(), cart->GetROMLength(), cart->GetHeader());
}

bool OverlayLoaded(const NDS& nds, u32 base, u32 size, const std::vector<u8>& image)
{
    return OverlayLoaded([&nds](u32 addr) { return nds.Debugger.Peek(0, addr, 4); }, base, size, image);
}

}
