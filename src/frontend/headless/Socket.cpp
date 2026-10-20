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

#include "Socket.h"

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int SockLen;
#define NATIVE(s) (SOCKET(s))
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef socklen_t SockLen;
#define NATIVE(s) (s)
#endif

namespace Headless::Net
{

static void EnsureStarted()
{
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

void CloseSocket(Socket s)
{
    if (s == InvalidSocket)
        return;
#ifdef _WIN32
    closesocket(NATIVE(s));
#else
    close(s);
#endif
}

static void SetNoDelay(Socket s)
{
    int on = 1;
    setsockopt(NATIVE(s), IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

Socket BindFirstFree(uint16_t preferred, uint16_t range, uint16_t& bound)
{
    EnsureStarted();
    uint32_t span = range == 0 ? 1 : range;
    uint32_t last = preferred == 0 ? 0 : std::min<uint32_t>(65535, uint32_t(preferred) + span - 1);
    for (uint32_t p = preferred; p <= last; p++)
    {
        Socket s = Socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (s == InvalidSocket)
            return InvalidSocket;
        int on = 1;
#ifdef _WIN32
        // SO_REUSEADDR on Windows lets two sockets share a port; refuse that.
        setsockopt(NATIVE(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof on);
#else
        // On POSIX this only skips TIME_WAIT from an earlier run; two listeners still conflict.
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(p));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(NATIVE(s), (sockaddr*)&a, sizeof a) == 0 && listen(NATIVE(s), 1) == 0)
        {
            SockLen len = sizeof a;
            getsockname(NATIVE(s), (sockaddr*)&a, &len); // the real port when preferred == 0
            bound = ntohs(a.sin_port);
            return s;
        }
        CloseSocket(s); // EADDRINUSE / WSAEADDRINUSE: try the next port
    }
    return InvalidSocket;
}

Socket Accept(Socket listener)
{
    Socket s = Socket(::accept(NATIVE(listener), nullptr, nullptr));
    if (s != InvalidSocket)
        SetNoDelay(s);
    return s;
}

Socket ConnectLoopback(uint16_t port)
{
    EnsureStarted();
    Socket s = Socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == InvalidSocket)
        return InvalidSocket;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(NATIVE(s), (sockaddr*)&a, sizeof a) != 0)
    {
        CloseSocket(s);
        return InvalidSocket;
    }
    SetNoDelay(s);
    return s;
}

bool WouldBlock()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool WaitFor(Socket s, bool write, int timeoutMs)
{
#ifdef _WIN32
    WSAPOLLFD fd = {};
    fd.fd = NATIVE(s);
    fd.events = write ? POLLWRNORM : POLLRDNORM;
    int r = WSAPoll(&fd, 1, timeoutMs);
#else
    pollfd fd = {};
    fd.fd = s;
    fd.events = write ? POLLOUT : POLLIN;
    int r = poll(&fd, 1, timeoutMs);
#endif
    return r > 0;
}

bool WaitReadable(Socket s, int timeoutMs)
{
    return WaitFor(s, false, timeoutMs);
}

bool SendAll(Socket s, const void* data, size_t len)
{
    const char* p = (const char*)data;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    while (len)
    {
        int n = (int)send(NATIVE(s), p, (int)std::min<size_t>(len, 1 << 20), flags);
        if (n < 0)
        {
            if (WouldBlock() && WaitFor(s, true, -1))
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

int Send(Socket s, const void* data, size_t len)
{
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    return (int)send(NATIVE(s), (const char*)data, (int)std::min<size_t>(len, 1 << 20), flags);
}

int Recv(Socket s, void* buf, size_t len)
{
    return (int)recv(NATIVE(s), (char*)buf, (int)std::min<size_t>(len, 1 << 20), 0);
}

bool SetNonBlocking(Socket s, bool nonBlocking)
{
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(NATIVE(s), FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
        return false;
    flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

uint32_t LocalAddress(Socket s)
{
    sockaddr_in a = {};
    SockLen len = sizeof a;
    if (getsockname(NATIVE(s), (sockaddr*)&a, &len) != 0)
        return 0;
    return ntohl(a.sin_addr.s_addr);
}

}
