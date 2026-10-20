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

// melonds_server: hosts consoles and the socket server (see Protocol.h). Uses only the public C API.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

#include "melonds_api.h"

namespace fs = std::filesystem;

static const int MaxConsoles = 16;

static const char* Usage =
    "Usage: melonds_server [options]\n"
    "\n"
    "  --port N            preferred port (default 7777); 0 lets the OS pick\n"
    "  --port-range N      ports to try from --port (default 32); 1 fails instead of searching\n"
    "  --port-file PATH    write the bound port to PATH once listening\n"
    "  --consoles N        consoles created at startup, 1-16 (default 1), all with --rom\n"
    "  --instance-base N   added to each console's id for MAC and save numbering (default 0)\n"
    "  --rom PATH          ROM loaded into every console      (env MELONDS_ROM)\n"
    "  --save PATH         save file of console 0; the others use <rom>.sav.N+1\n"
    "  --bios9 PATH        ARM9 BIOS                          (env MELONDS_BIOS9)\n"
    "  --bios7 PATH        ARM7 BIOS                          (env MELONDS_BIOS7)\n"
    "  --firmware PATH     firmware image                     (env MELONDS_FIRMWARE)\n"
    "  --local-dir PATH    directory for local files          (env MELONDS_LOCAL_DIR)\n"
    "  --name NAME         label returned in hello and the ready line (default melonds)\n"
    "  --help              show this text\n"
    "\n"
    "Prints one line when listening: MELONDS_READY port=<port> pid=<pid> name=<name>\n"
    "Errors print MELONDS_ERROR <message>; exit code 2 if no port is free, 1 for other errors.\n"
    "Runs until a client sends `shutdown` or the process gets Ctrl+C / SIGTERM; then exits 0.\n";

struct Options
{
    long Port = 7777;
    long PortRange = 32;
    std::string PortFile;
    long Consoles = 1;
    long InstanceBase = 0;
    std::string Rom, Save, Bios9, Bios7, Firmware, LocalDir, Name;
    bool Help = false;
};

static std::atomic<bool> Interrupted {false};

#ifdef _WIN32
static BOOL WINAPI OnConsoleEvent(DWORD)
{
    Interrupted = true;
    return TRUE;
}
#else
static void OnSignal(int)
{
    Interrupted = true;
}
#endif

static void InstallHandlers()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
#else
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
#endif
}

static std::string EnvOr(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Prints the error line and returns the exit code.
static int Fail(const std::string& message, int code = 1)
{
    printf("MELONDS_ERROR %s\n", message.c_str());
    fflush(stdout);
    return code;
}

static bool ParseNumber(const std::string& text, long min, long max, long& out)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    long v = strtol(text.c_str(), &end, 10);
    if (*end != '\0' || v < min || v > max)
        return false;
    out = v;
    return true;
}

// Returns an error message, or "" on success.
static std::string ParseArgs(int argc, char** argv, Options& o)
{
    o.Rom = EnvOr("MELONDS_ROM");
    o.Bios9 = EnvOr("MELONDS_BIOS9");
    o.Bios7 = EnvOr("MELONDS_BIOS7");
    o.Firmware = EnvOr("MELONDS_FIRMWARE");
    o.LocalDir = EnvOr("MELONDS_LOCAL_DIR");

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        std::string value;
        bool hasValue = false;
        size_t eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos)
        {
            value = arg.substr(eq + 1);
            arg.resize(eq);
            hasValue = true;
        }
        if (arg == "--help" || arg == "-h")
        {
            o.Help = true;
            return "";
        }

        auto take = [&](const char* name) -> bool {
            if (arg != name)
                return false;
            if (!hasValue)
            {
                if (i + 1 >= argc)
                    throw std::string("missing value for ") + name;
                value = argv[++i];
            }
            return true;
        };
        auto number = [&](const char* name, long min, long max, long& out) {
            if (!ParseNumber(value, min, max, out))
                throw std::string("bad value for ") + name + ": '" + value + "' (" + std::to_string(min) +
                      "-" + std::to_string(max) + ")";
        };

        try
        {
            if (take("--port")) number("--port", 0, 65535, o.Port);
            else if (take("--port-range")) number("--port-range", 1, 65535, o.PortRange);
            else if (take("--port-file")) o.PortFile = value;
            else if (take("--consoles")) number("--consoles", 1, MaxConsoles, o.Consoles);
            else if (take("--instance-base")) number("--instance-base", 0, 1000000, o.InstanceBase);
            else if (take("--rom")) o.Rom = value;
            else if (take("--save")) o.Save = value;
            else if (take("--bios9")) o.Bios9 = value;
            else if (take("--bios7")) o.Bios7 = value;
            else if (take("--firmware")) o.Firmware = value;
            else if (take("--local-dir")) o.LocalDir = value;
            else if (take("--name")) o.Name = value;
            else return "unknown argument: " + std::string(argv[i]);
        }
        catch (const std::string& message)
        {
            return message;
        }
    }
    return "";
}

// Temp file + rename, so a reader never sees a partial number.
static bool WritePortFile(const std::string& path, int port)
{
    fs::path target = fs::u8path(path);
    fs::path temp = target;
    temp += ".tmp" + std::to_string(GETPID());
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << port << "\n";
        if (!f.flush())
            return false;
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec)
    {
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
    }
    if (ec)
    {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

static int Run(const Options& o)
{
    if (!o.LocalDir.empty())
        mds_set_local_dir(fs::absolute(fs::u8path(o.LocalDir)).u8string().c_str());
    if (mds_set_instance_base((int)o.InstanceBase) < 0)
        return Fail(mds_error());

    auto opt = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
    std::vector<int> ids;
    for (long n = 0; n < o.Consoles; n++)
    {
        int id = mds_create(opt(o.Bios9), opt(o.Bios7), opt(o.Firmware), nullptr, 1);
        if (id < 0)
            return Fail(mds_error());
        ids.push_back(id);
        if (!o.Rom.empty())
        {
            // --save names console 0's save file only; the others keep the default numbering
            if (mds_load_rom(id, o.Rom.c_str(), n == 0 ? opt(o.Save) : nullptr) < 0)
                return Fail(mds_error());
        }
        else if (mds_reset(id) < 0)     // starts the NDS (FreeBIOS, no cart)
            return Fail(mds_error());
    }

    std::string name = o.Name.empty() ? "melonds" : o.Name;
    mds_serve_name(name.c_str());
    int bound = 0;
    if (mds_serve((int)o.Port, (int)o.PortRange, &bound) < 0)
    {
        std::string message = mds_error();
        return Fail(message, message.rfind("no free port", 0) == 0 ? 2 : 1);
    }

    if (!o.PortFile.empty() && !WritePortFile(o.PortFile, bound))
    {
        mds_serve_stop();
        return Fail("cannot write port file: " + o.PortFile);
    }

    printf("MELONDS_READY port=%d pid=%d name=%s\n", bound, (int)GETPID(), name.c_str());
    fflush(stdout);

    while (!Interrupted && !mds_serve_wait(100))
        ;

    mds_serve_stop();
    return 0;
}

int main(int argc, char** argv)
{
    Options o;
    std::string error = ParseArgs(argc, argv, o);
    if (!error.empty())
        return Fail(error);
    if (o.Help)
    {
        fputs(Usage, stdout);
        return 0;
    }

    InstallHandlers();
    int code = Run(o);
    for (int id = 0; id < MaxConsoles; id++)
        mds_destroy(id);
    return code;
}
