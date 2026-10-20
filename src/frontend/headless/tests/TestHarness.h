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

#ifndef HEADLESS_TESTHARNESS_H
#define HEADLESS_TESTHARNESS_H

// Minimal header-only test harness. Define TEST_HARNESS_MAIN before including
// in exactly one translation unit to get main(). An optional argv[1] selects
// tests whose name contains it.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace TestHarness
{

struct Case
{
    const char* Name;
    void (*Fn)();
};

inline std::vector<Case>& Registry()
{
    static std::vector<Case> cases;
    return cases;
}

inline std::atomic<int>& Failures()
{
    static std::atomic<int> failures {0};
    return failures;
}

struct Registrar
{
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline void Fail(const char* file, int line, const std::string& what)
{
    Failures()++;
    std::fprintf(stderr, "%s:%d: FAILED: %s\n", file, line, what.c_str());
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line)
{
    if (!(a == b))
        Fail(file, line, std::string(ea) + " == " + eb + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")");
}

inline int RunAll(int argc, char** argv)
{
    int ran = 0, failed = 0;
    for (auto& c : Registry())
    {
        if (argc > 1 && !std::strstr(c.Name, argv[1]))
            continue;
        int before = Failures();
        std::printf("[ RUN  ] %s\n", c.Name);
        std::fflush(stdout);
        c.Fn();
        ran++;
        if (Failures() != before)
        {
            failed++;
            std::printf("[ FAIL ] %s\n", c.Name);
        }
        else
            std::printf("[  OK  ] %s\n", c.Name);
    }
    std::printf("%d test(s) run, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}

}

#define TEST(name) \
    static void test_##name(); \
    static ::TestHarness::Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) ::TestHarness::Fail(__FILE__, __LINE__, #cond); } while (0)

#define CHECK_EQ(a, b) ::TestHarness::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

#ifdef TEST_HARNESS_MAIN
int main(int argc, char** argv)
{
    return ::TestHarness::RunAll(argc, argv);
}
#endif

#endif
