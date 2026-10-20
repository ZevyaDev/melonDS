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

#ifndef HEADLESS_SERVER_H
#define HEADLESS_SERVER_H

#include <string>

namespace Headless
{

// The process's socket server (Protocol.h). One server thread, one client at a time,
// serving every console in the process. These back mds_serve and friends.

// port 0: the OS picks. Otherwise the first free port in [port, port + range), range 0 = 32.
bool ServerStart(int port, int range, int& bound, std::string& error);
void ServerStop();
// 1 once a client has sent `shutdown` (since ServerStart), else 0 after timeoutMs (< 0 = forever).
int ServerWait(int timeoutMs);
void ServerSetName(const std::string& name);

}

#endif
