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

#ifndef HEADLESS_SOCKET_H
#define HEADLESS_SOCKET_H

// Thin cross-platform TCP wrappers (Winsock on Windows, POSIX elsewhere) for
// the loopback socket server. Winsock is started on first use. This header
// does not include any system socket header.

#include <cstddef>
#include <cstdint>

namespace Headless::Net
{

#ifdef _WIN32
typedef uintptr_t Socket;
constexpr Socket InvalidSocket = ~Socket(0);
#else
typedef int Socket;
constexpr Socket InvalidSocket = -1;
#endif

void CloseSocket(Socket s);

// Bind 127.0.0.1 on the first free port in [preferred, preferred + range) and
// listen with a backlog of 1. preferred == 0 lets the OS choose; range == 1
// means strict (no searching). Returns the listening socket and stores the
// real port in `bound`, or returns InvalidSocket if every port is taken.
// Ports are claimed exclusively: a port already held by another listener is
// never shared (SO_EXCLUSIVEADDRUSE on Windows, plain SO_REUSEADDR semantics
// on POSIX, where it only skips TIME_WAIT).
Socket BindFirstFree(uint16_t preferred, uint16_t range, uint16_t& bound);

// Accept one pending connection (blocking unless the listener is
// non-blocking). Returns InvalidSocket on failure. TCP_NODELAY is set.
Socket Accept(Socket listener);

// Connect to 127.0.0.1:port. TCP_NODELAY is set.
Socket ConnectLoopback(uint16_t port);

// Send every byte, waiting out a full send buffer even on a non-blocking
// socket. Returns false on error or peer close.
bool SendAll(Socket s, const void* data, size_t len);

// recv(): > 0 bytes read, 0 peer closed, < 0 error (or would block on a
// non-blocking socket; check WouldBlock()).
int Recv(Socket s, void* buf, size_t len);

// True if the last failed call on this thread failed only because it would block.
bool WouldBlock();

// Wait until the socket is readable (data, a pending accept, or a close).
// timeoutMs < 0 waits forever. Returns false on timeout or error.
bool WaitReadable(Socket s, int timeoutMs);

bool SetNonBlocking(Socket s, bool nonBlocking);

// Local IPv4 address of the socket in host byte order (0x7F000001 for loopback), 0 on error.
uint32_t LocalAddress(Socket s);

}

#endif
