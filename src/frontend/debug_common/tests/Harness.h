// Minimal test harness for debug_common_tests.
#ifndef DEBUG_COMMON_TEST_HARNESS_H
#define DEBUG_COMMON_TEST_HARNESS_H

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace th
{
struct Case { const char* Name; std::function<void()> Fn; };
inline std::vector<Case>& Cases() { static std::vector<Case> c; return c; }
inline int& Failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { Cases().push_back({n, std::move(f)}); } };

template <typename T> std::string Show(const T& v) { std::ostringstream s; s << v; return s.str(); }
inline std::string Show(unsigned char v) { return std::to_string((int)v); }
}

#define TEST(name) \
    static void name(); \
    static th::Reg reg_##name(#name, name); \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); th::Failures()++; } } while (0)

#define CHECK_EQ(a, b) \
    do { auto&& va_ = (a); auto&& vb_ = (b); if (!(va_ == vb_)) { \
        std::printf("  FAIL %s:%d: %s == %s (got '%s', want '%s')\n", __FILE__, __LINE__, #a, #b, \
                    th::Show(va_).c_str(), th::Show(vb_).c_str()); th::Failures()++; } } while (0)

#define TEST_MAIN() \
    int main() \
    { \
        for (auto& c : th::Cases()) \
        { \
            int before = th::Failures(); \
            std::printf("[ RUN ] %s\n", c.Name); \
            c.Fn(); \
            std::printf("[ %s ] %s\n", th::Failures() == before ? " OK " : "FAIL", c.Name); \
        } \
        std::printf("%d failure(s)\n", th::Failures()); \
        return th::Failures() ? 1 : 0; \
    }

#endif
