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

// The Action Replay text parser behind mds_cheat_add (Cheats.cpp). The codes running in the core's engine are
// covered by tools/melonds-mcp/tests/test_cheats.py.

#include <string>
#include <vector>

#include "TestHarness.h"
#include "Cheats.h"

using namespace Headless;

static bool Parse(const std::string& text, std::vector<uint32_t>& words, std::string& error)
{
    words.clear();
    error.clear();
    return ParseCheatCode(text, words, error);
}

static bool Contains(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

TEST(CheatParsesPairsInOrder)
{
    std::vector<uint32_t> w;
    std::string err;
    CHECK(Parse("020F6D48 0098967F\n120F5510 0000270F", w, err));
    CHECK_EQ(w.size(), size_t(4));
    CHECK_EQ(w[0], 0x020F6D48u);
    CHECK_EQ(w[1], 0x0098967Fu);
    CHECK_EQ(w[2], 0x120F5510u);
    CHECK_EQ(w[3], 0x0000270Fu);
}

TEST(CheatAcceptsSpacingCaseAndLineEnds)
{
    std::vector<uint32_t> w;
    std::string err;
    CHECK(Parse("\r\n  020f6d48\t0098967f  \r\n\r\n94000130 FFFB0000\r\nD2000000 00000000\r\n", w, err));
    CHECK_EQ(w.size(), size_t(6));
    CHECK_EQ(w[0], 0x020F6D48u);
    CHECK_EQ(w[1], 0x0098967Fu);
    CHECK_EQ(w[5], 0u);
}

TEST(CheatRejectsEmptyText)
{
    std::vector<uint32_t> w;
    std::string err;
    CHECK(!Parse("", w, err));
    CHECK(Contains(err, "empty"));
    CHECK(!Parse("  \n\t\n", w, err));
    CHECK(Contains(err, "empty"));
}

TEST(CheatRejectsMalformedLinesWithTheLineNumber)
{
    std::vector<uint32_t> w;
    std::string err;
    for (const char* bad : {"020F6D48", "020F6D48 0098967", "020F6D48 0098967F 00000000", "0x20F6D48 0098967F",
                            "020F6D4G 0098967F", "020F6D48:0098967F"})
    {
        CHECK(!Parse(std::string("94000130 FFFB0000\n") + bad, w, err));
        CHECK(Contains(err, "line 2:"));
        CHECK(Contains(err, "XXXXXXXX YYYYYYYY"));
    }
    CHECK(w.empty());
}

TEST(CheatCountsBlankLinesInLineNumbers)
{
    std::vector<uint32_t> w;
    std::string err;
    CHECK(!Parse("94000130 FFFB0000\n\n\nnot a code", w, err));
    CHECK(Contains(err, "line 4:"));
}

TEST(CheatRejectsOpcodesTheEngineDoesNotRun)
{
    std::vector<uint32_t> w;
    std::string err;
    CHECK(!Parse("C4000000 00000000", w, err));
    CHECK(Contains(err, "C4"));
    CHECK(Contains(err, "not supported"));
    for (const char* op : {"C1000000 00000000", "C7000000 00000000", "CF000000 00000000", "DD000000 00000000",
                           "DF000000 00000000"})
    {
        CHECK(!Parse(op, w, err));
        CHECK(Contains(err, "unknown opcode"));
    }
    CHECK(!Parse("D4000009 00000001", w, err));
    CHECK(Contains(err, "D4 data operation 09"));
    for (const char* ok : {"C0000000 00000003\nD2000000 00000000", "C5000000 00010000\nD0000000 00000000",
                           "C6000000 02000000", "D4000008 00000002", "DC000000 00000004", "F2000000 00000010",
                           "B2000000 00000000", "A4000130 FCFF0300"})
        CHECK(Parse(ok, w, err));
}

TEST(CheatEOpcodeNeedsItsDataLines)
{
    std::vector<uint32_t> w;
    std::string err;
    // 0x18 bytes = 3 data lines
    CHECK(Parse("E21B72DC 00000018\nE59F100C E28D001C\nE3A0200B EBF929D4\nEA000080 021B72F4\nD2000000 00000000", w,
                err));
    CHECK_EQ(w.size(), size_t(10));
    // 9 bytes = 2 lines (the last padded)
    CHECK(Parse("E2000000 00000009\n11111111 22222222\n33000000 00000000", w, err));
    CHECK(!Parse("E2000000 00000018\nE59F100C E28D001C\nE3A0200B EBF929D4", w, err));
    CHECK(Contains(err, "line 1:"));
    CHECK(Contains(err, "24 bytes (3 lines of data) but only 2 lines follow"));
    // data lines are data: an "opcode" there is not checked
    CHECK(Parse("E2000000 00000008\nC4000000 DD000000", w, err));
    CHECK(Parse("E2000000 00000000", w, err));
}

TEST(CheatParsesARealDQ9Code)
{
    // Dragon Quest IX (USA) "Wild Enemy Modifier": conditions, data-register moves and two E copies
    const char* code =
        "52388C34 FFFFFFFF\n92388C62 00000238\n94000130 FFF60000\nDA000000 02388C5C\nD7000000 02000600\n"
        "D2000000 00000000\n521B72D8 E3570000\nE21B72DC 00000024\nBA000011 E1A01007\nE28A002C EBFAF398\n"
        "E28A802C E3A07000\nE28D201C E28D0024\nEA000006 00000000\nA4000130 FCFF0300\n021B72F8 00000000\n"
        "021B72FC 00000001\nE21B72DC 00000018\nE59F100C E28D001C\nE3A0200B EBF929D4\nEA000080 021B72F4\n"
        "DA000000 02000600\nD7000000 021B72F4\nD2000000 00000000";
    std::vector<uint32_t> w;
    std::string err;
    CHECK(Parse(code, w, err));
    CHECK_EQ(w.size(), size_t(46));
    CHECK_EQ(w[44], 0xD2000000u);
}

TEST(CheatRejectsOverlongCodes)
{
    std::string text;
    for (size_t i = 0; i <= MaxCheatWords / 2; i++)
        text += "02000000 00000000\n";
    std::vector<uint32_t> w;
    std::string err;
    CHECK(!Parse(text, w, err));
    CHECK(Contains(err, "longer than"));
}
