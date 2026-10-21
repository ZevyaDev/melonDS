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

#include <cstring>
#include <filesystem>
#include <fstream>

#include "Harness.h"
#include "DebugSymbols.h"
#include "Disassembler.h"
#include "NDS_Header.h"
#include "debug/RomOverlays.h"

#if __has_include(<cxxabi.h>)
#define HAVE_DEMANGLE
#endif

namespace fs = std::filesystem;
using namespace melonDS;

namespace
{

struct TempDir
{
    fs::path Path;
    explicit TempDir(const char* tag)
    {
        Path = fs::temp_directory_path() / (std::string("melonds_dc_") + tag);
        fs::remove_all(Path);
        fs::create_directories(Path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(Path, ec); }
    std::string file(const std::string& rel) const { return (Path / rel).generic_string(); }
    void write(const std::string& rel, const std::string& text) const
    {
        fs::path p = Path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << text;
    }
};

}

TEST(PlainSymbolFile)
{
    TempDir dir("plain");
    dir.write("my.game.sym",
        "02000000 main 40\n"
        "02000041 thumbfn 0x10\n"
        "# comment\n"
        "zzzz bad\n"
        "justone\n"
        "\n"
        "02001000 _ZN3FooC1Ev\r\n"
        "   02002000\t spaced   8   \n");
    DebugSymbols syms;
    CHECK(syms.empty());
    std::string err;
    CHECK(syms.load(dir.file("my.game.sym"), err));
    CHECK(!syms.empty());
    CHECK_EQ(syms.source(), dir.file("my.game.sym"));
    auto set = syms.set();
    CHECK(set != nullptr);
    if (!set)
        return;
    CHECK_EQ(set->Modules.size(), (size_t)1);
    CHECK_EQ(set->SymbolCount, 4);
    const auto& mod = set->Modules[0];
    CHECK_EQ(mod.Name, std::string("my"));
    CHECK_EQ(mod.Overlay, -1);
    CHECK_EQ(mod.Base, 0x02000000u);
    CHECK_EQ(mod.Size, 0x2008u);
    CHECK_EQ(mod.Symbols.size(), (size_t)4);
    CHECK_EQ(mod.Symbols[0].Name, std::string("main"));
    CHECK_EQ(mod.Symbols[0].Size, 40u);
    CHECK_EQ(mod.Symbols[0].Kind, std::string("function"));
    CHECK(!mod.Symbols[0].Thumb);
    CHECK_EQ(mod.Symbols[1].Addr, 0x02000040u);
    CHECK(mod.Symbols[1].Thumb);
    CHECK_EQ(mod.Symbols[1].Size, 16u);
    CHECK_EQ(mod.Symbols[3].Name, std::string("spaced"));
    CHECK_EQ(mod.Symbols[3].Size, 8u);

    // lookup masks the Thumb bit and honours sizes (size 0 counts as 1 byte)
    CHECK(syms.lookup(0x02000000) != nullptr && syms.lookup(0x02000000)->Name == "main");
    CHECK(syms.lookup(0x02000027) != nullptr);
    CHECK(syms.lookup(0x02000028) == nullptr);
    CHECK(syms.lookup(0x02000041) != nullptr && syms.lookup(0x02000041)->Name == "thumbfn");
    CHECK(syms.lookup(0x02001000) != nullptr);
    CHECK(syms.lookup(0x02001002) == nullptr);
    CHECK(syms.lookup(0x01000000) == nullptr);

    CHECK_EQ(syms.describe(0x02000000), std::string("main"));
    CHECK_EQ(syms.describe(0x02000044), std::string("thumbfn+0x4"));
    CHECK_EQ(syms.describe(0x02000030), std::string(""));
    CHECK_EQ(syms.exact(0x02000040), std::string("thumbfn"));
    CHECK_EQ(syms.exact(0x02000041), std::string("thumbfn"));
    CHECK_EQ(syms.exact(0x02000044), std::string(""));
    CHECK(syms.moduleOf(syms.lookup(0x02000000)) == &set->Modules[0]);
    CHECK(syms.moduleOf(nullptr) == nullptr);

    u32 a = 0;
#ifdef HAVE_DEMANGLE
    CHECK_EQ(mod.Symbols[2].Pretty, std::string("Foo::Foo()"));
    CHECK_EQ(syms.describe(0x02001000), std::string("Foo::Foo()"));
    syms.setDemangle(false);
    CHECK_EQ(syms.describe(0x02001000), std::string("_ZN3FooC1Ev"));
    syms.setDemangle(true);
    CHECK(syms.resolve("Foo::Foo()", a) && a == 0x02001000);
    CHECK(syms.resolve("Foo::Foo", a) && a == 0x02001000);
#endif
    CHECK_EQ(mod.Symbols[0].Pretty, std::string("main"));

    CHECK(syms.resolve("main", a) && a == 0x02000000);
    CHECK(syms.resolve("thumbfn", a) && a == 0x02000040);
    CHECK(!syms.resolve("nosuch", a));

    auto hits = syms.search("MAI", 10);
    CHECK_EQ(hits.size(), (size_t)1);
    CHECK(!hits.empty() && hits[0]->Name == "main");
    CHECK_EQ(syms.search("n", 2).size(), (size_t)2);
    CHECK_EQ(syms.search("zzz-none", 5).size(), (size_t)0);

    CHECK(syms.parseAddress("main", a) && a == 0x02000000);
    CHECK(syms.parseAddress("main+0x10", a) && a == 0x02000010);
    CHECK(syms.parseAddress("  main + 10 ", a) && a == 0x02000010);
    CHECK(syms.parseAddress("0x02000040", a) && a == 0x02000040);
    CHECK(syms.parseAddress("0X2000040", a) && a == 0x02000040);
    CHECK(syms.parseAddress("deadbeef", a) && a == 0xDEADBEEFu);
    CHECK(syms.parseAddress("0x20+4", a) && a == 0x24);
    CHECK(!syms.parseAddress("", a));
    CHECK(!syms.parseAddress("   ", a));
    CHECK(!syms.parseAddress("nosuchsym", a));
    CHECK(!syms.parseAddress("zz", a));

    syms.clear();
    CHECK(syms.empty());
    CHECK(syms.source().empty());
    CHECK(syms.lookup(0x02000000) == nullptr);
    CHECK(!syms.resolve("main", a));
    CHECK_EQ(syms.describe(0x02000000), std::string(""));
}

TEST(LoadErrors)
{
    TempDir dir("errors");
    DebugSymbols syms;
    std::string err;
    CHECK(!syms.load(dir.file("missing.sym"), err));
    CHECK_EQ(err, "not found: " + dir.file("missing.sym"));

    dir.write("empty.sym", "# nothing\n");
    CHECK(!syms.load(dir.file("empty.sym"), err));
    CHECK_EQ(err, "no symbols in " + dir.file("empty.sym"));

    dir.write("nomain/config.yaml", "overlays:\n  - id: 0\n    name: ov0\n");
    CHECK(!syms.load(dir.file("nomain/config.yaml"), err));
    CHECK_EQ(err, "no main_module in " + dir.file("nomain/config.yaml"));

    dir.write("ok.sym", "02000000 f 4\n");
    CHECK(syms.load(dir.file("ok.sym"), err));
    // a failed load leaves the current set alone
    CHECK(!syms.load(dir.file("empty.sym"), err));
    CHECK(!syms.empty());
    CHECK_EQ(syms.source(), dir.file("ok.sym"));
}

static void WriteDsdProject(const TempDir& dir)
{
    dir.write("proj/config.yaml",
        "# dsd config\n"
        "---\n"
        "rom_config: rom/rom_config.yaml\n"
        "main_module:\n"
        "  name: main\n"
        "  symbols: main.txt\n"
        "autoloads:\n"
        "  - name: itcm\n"
        "    symbols: itcm.txt\n"
        "overlays:\n"
        "  - id: 0\n"
        "    name: 'ov0'\n"
        "    symbols: ov0.txt\n"
        "  - id: 1\n"
        "    name: \"ov1\"\n"
        "    symbols: ov1.txt\n");
    dir.write("proj/main.txt",
        "main_func kind:function(size=0x20) addr:0x02000100\n"
        "gVar kind:data(word[4]) addr:0x02000200\n"
        "gByte kind:data(byte) addr:0x02000210\n"
        "gShort kind:data(short[]) addr:0x02000212\n"
        "lbl kind:label addr:0x02000300\n"
        "th kind:function(thumb,size=8) addr:0x02000401\n"
        "broken_no_addr kind:function\n"
        "shared kind:function(size=4) addr:0x02000500\n");
    dir.write("proj/itcm.txt", "itcm_fn kind:function(size=4) addr:0x01FF8000\n");
    dir.write("proj/ov0.txt", "ov0_fn kind:function(size=0x10,thumb) addr:0x02100010\nshared kind:function(size=4) addr:0x02100020\n");
    dir.write("proj/ov1.txt", "ov1_fn kind:function(size=0x10) addr:0x02100010\n");
    dir.write("proj/rom/rom_config.yaml", "x: 1\n");
    dir.write("proj/rom/arm9_overlays/overlays.yaml",
        "overlays:\n"
        "  - id: 0\n"
        "    base_address: 0x02100000\n"
        "    code_size: 0x100\n"
        "    bss_size: 0x20\n"
        "    file_name: ov0.bin\n");
    std::string image;
    for (int i = 0; i < 0x100; i++)
        image += (char)((i * 7 + 3) & 0xFF);
    dir.write("proj/rom/arm9_overlays/ov0.bin", image);
}

TEST(Labels)
{
    TempDir dir("labels");
    WriteDsdProject(dir);
    dir.write("my.labels",
        "# address name [module]\n"
        "02000100 renamed_main\n"
        "0x02000450 new_fn\n"
        "02000501 thumb_label main\n"
        "02100010 ov1_named OV1\n"
        "02100030 ov0_new ov0\n"
        "02200000 nowhere\n"
        "02000000 bad_module ov9\n"
        "zz skipped\n"
        "justone\n");
    DebugSymbols syms;
    std::string err;
    CHECK(syms.load(dir.file("proj"), err));
    std::string base = syms.source();
    CHECK(syms.loadLabels(dir.file("my.labels"), err));
    CHECK_EQ(syms.labelCount(), 7);
    CHECK_EQ(syms.labelsUnmatched(), 2);
    CHECK_EQ(syms.labelsSource(), dir.file("my.labels"));
    CHECK_EQ(syms.source(), base);

    u32 a = 0;
    // a label renames the symbol at its address; the old name still resolves
    CHECK_EQ(syms.exact(0x02000100), std::string("renamed_main"));
    CHECK(syms.resolve("renamed_main", a) && a == 0x02000100);
    CHECK(syms.resolve("main_func", a) && a == 0x02000100);
    // or adds a symbol where there was none
    CHECK(syms.resolve("new_fn", a) && a == 0x02000450);
    CHECK(syms.lookup(0x02000450) && syms.lookup(0x02000450)->Kind == "label");
    // the Thumb bit is dropped from the address and the existing symbol renamed
    CHECK_EQ(syms.exact(0x02000500), std::string("thumb_label"));
    // module names match case-insensitively; overlay labels show only while loaded
    CHECK(syms.lookup(0x02100010) == nullptr);
    syms.setLoaded({1});
    CHECK_EQ(syms.exact(0x02100010), std::string("ov1_named"));
    syms.setLoaded({0});
    CHECK_EQ(syms.exact(0x02100010), std::string("ov0_fn"));
    CHECK_EQ(syms.exact(0x02100030), std::string("ov0_new"));
    syms.setLoaded({});

    syms.clearLabels();
    CHECK(syms.labelsSource().empty());
    CHECK_EQ(syms.exact(0x02000100), std::string("main_func"));
    CHECK(!syms.resolve("renamed_main", a));
    CHECK(!syms.resolve("new_fn", a));

    // labels without symbols form their own module
    DebugSymbols only;
    dir.write("only.labels", "02000000 entry\n02000010 other ov0\n");
    CHECK(only.loadLabels(dir.file("only.labels"), err));
    CHECK(!only.empty());
    CHECK_EQ(only.labelsUnmatched(), 1);
    CHECK(only.resolve("entry", a) && a == 0x02000000);

    CHECK(!syms.loadLabels(dir.file("missing.labels"), err));
    CHECK_EQ(err, "cannot read " + dir.file("missing.labels"));
    dir.write("empty.labels", "# none\n");
    CHECK(!syms.loadLabels(dir.file("empty.labels"), err));
    CHECK_EQ(err, "no labels in " + dir.file("empty.labels"));
}

TEST(DsdProject)
{
    TempDir dir("dsd");
    WriteDsdProject(dir);
    DebugSymbols syms;
    std::string err;
    // loading a directory finds config.yaml in it
    CHECK(syms.load(dir.file("proj"), err));
    CHECK_EQ(fs::path(syms.source()).filename().string(), std::string("config.yaml"));
    auto set = syms.set();
    CHECK(set != nullptr);
    if (!set)
        return;
    CHECK_EQ(set->Modules.size(), (size_t)4);
    if (set->Modules.size() != 4)
        return;
    CHECK_EQ(set->Modules[0].Name, std::string("main"));
    CHECK_EQ(set->Modules[1].Name, std::string("itcm"));
    CHECK_EQ(set->Modules[2].Name, std::string("ov0"));
    CHECK_EQ(set->Modules[2].Overlay, 0);
    CHECK_EQ(set->Modules[3].Name, std::string("ov1"));
    CHECK_EQ(set->Modules[3].Overlay, 1);
    CHECK_EQ(set->SymbolCount, 7 + 1 + 2 + 1);

    const auto& main = set->Modules[0];
    CHECK_EQ(main.Symbols.size(), (size_t)7);
    auto find = [&](const char* n) -> const DebugSymbols::Symbol* {
        for (auto& s : main.Symbols) if (s.Name == n) return &s;
        return nullptr;
    };
    CHECK(find("main_func") && find("main_func")->Size == 0x20 && find("main_func")->Kind == "function");
    CHECK(find("gVar") && find("gVar")->Size == 16 && find("gVar")->Kind == "data");
    CHECK(find("gByte") && find("gByte")->Size == 1);
    CHECK(find("gShort") && find("gShort")->Size == 2);
    CHECK(find("lbl") && find("lbl")->Size == 0 && find("lbl")->Kind == "label");
    CHECK(find("th") && find("th")->Thumb && find("th")->Size == 8 && find("th")->Addr == 0x02000401u);
    CHECK(find("broken_no_addr") == nullptr);
    CHECK_EQ(main.Base, 0x02000100u);
    CHECK_EQ(main.Size, 0x02000500u + 4 - 0x02000100u);

    const auto& ov0 = set->Modules[2];
    CHECK_EQ(ov0.Base, 0x02100000u);
    CHECK_EQ(ov0.Size, 0x120u);
    CHECK_EQ(ov0.Image.size(), (size_t)0x100);
    CHECK(ov0.covers(0x02100000) && ov0.covers(0x0210011F) && !ov0.covers(0x02100120));
    const auto& ov1 = set->Modules[3];
    CHECK_EQ(ov1.Base, 0x02100010u);
    CHECK_EQ(ov1.Size, 0x10u);
    CHECK(ov1.Image.empty());
    CHECK(ov0.find(0x02100014) != nullptr && ov0.find(0x02100014)->Name == "ov0_fn");
    CHECK(ov0.find(0x02100004) == nullptr);

    // overlay handling: only loaded overlays are visible to lookup
    CHECK(syms.lookup(0x02100010) == nullptr);
    CHECK(syms.lookup(0x01FF8000) != nullptr);
    syms.setLoaded({1});
    CHECK_EQ(syms.loaded().size(), (size_t)1);
    CHECK(syms.lookup(0x02100010) && syms.lookup(0x02100010)->Name == "ov1_fn");
    CHECK_EQ(syms.describe(0x02100010), std::string("ov1_fn [ov1]"));
    CHECK_EQ(syms.describe(0x02100014), std::string("ov1_fn+0x4 [ov1]"));
    CHECK_EQ(syms.describe(0x02100014, false), std::string("ov1_fn+0x4"));
    CHECK_EQ(syms.describe(0x02000100), std::string("main_func"));
    syms.setLoaded({0});
    CHECK(syms.lookup(0x02100010) && syms.lookup(0x02100010)->Name == "ov0_fn");
    syms.setLoaded({0, 1});
    CHECK(syms.lookup(0x02100010) != nullptr);
    syms.setLoaded({});

    // resolve prefers a main/loaded candidate, else falls back to the most recently added
    u32 a = 0;
    CHECK(syms.resolve("shared", a) && a == 0x02000500);
    CHECK(syms.resolve("ov1_fn", a) && a == 0x02100010);
    CHECK(syms.resolve("ov0_fn", a) && a == 0x02100010);

    // overlay detection against memory
    std::function<u32(u32)> match = [&](u32 addr) -> u32 {
        u32 off = addr - 0x02100000;
        u32 v = 0;
        if (off + 4 <= 0x100)
            memcpy(&v, ov0.Image.data() + off, 4);
        return v;
    };
    CHECK(DebugSymbols::detect(*set, match) == std::vector<int>({0}));
    std::function<u32(u32)> blank = [](u32) -> u32 { return 0; };
    CHECK(DebugSymbols::detect(*set, blank).empty());
}

TEST(IndependentTables)
{
    TempDir dir("indep");
    dir.write("a.sym", "02000000 only_a 4\n");
    dir.write("b.sym", "02000000 only_b 4\n");
    DebugSymbols a, b;
    std::string err;
    CHECK(a.load(dir.file("a.sym"), err));
    CHECK(b.load(dir.file("b.sym"), err));
    CHECK_EQ(a.describe(0x02000000), std::string("only_a"));
    CHECK_EQ(b.describe(0x02000000), std::string("only_b"));
    a.clear();
    CHECK(!b.empty());
}

TEST(DisasmArm)
{
    Disassembler dis;
    u8 b[4];
    auto arm = [&](u32 pc, u32 op) { memcpy(b, &op, 4); return dis.decode(pc, b, 4, false); };

    DisasmLine l = arm(0x02000000, 0xEA000000);
    CHECK_EQ(l.Size, (u8)4);
    CHECK_EQ(l.Opcode, 0xEA000000u);
    CHECK(l.HasTarget && !l.IsCall && !l.IsLiteral);
    CHECK_EQ(l.Target, 0x02000008u);
    if (Disassembler::available())
        CHECK_EQ(l.Mnemonic, std::string("b"));

    l = arm(0x02000000, 0xEB000001);
    CHECK(l.HasTarget && l.IsCall);
    CHECK_EQ(l.Target, 0x0200000Cu);
    if (Disassembler::available())
        CHECK_EQ(l.Mnemonic, std::string("bl"));

    l = arm(0x02000000, 0xEAFFFFFE);
    CHECK_EQ(l.Target, 0x02000000u);

    l = arm(0x02000000, 0xFA000000);
    CHECK(l.HasTarget && l.IsCall);
    CHECK_EQ(l.Target, 0x02000008u);
    l = arm(0x02000000, 0xFB000000);
    CHECK_EQ(l.Target, 0x0200000Au);

    l = arm(0x02000000, 0xE59F0004);
    CHECK(l.HasTarget && l.IsLiteral && !l.IsCall);
    CHECK_EQ(l.Target, 0x0200000Cu);
    l = arm(0x02000100, 0xE51F0004);
    CHECK(l.IsLiteral);
    CHECK_EQ(l.Target, 0x02000104u);

    l = arm(0x02000000, 0xE3A00001);
    CHECK(!l.HasTarget);
    if (Disassembler::available())
    {
        CHECK_EQ(l.Mnemonic, std::string("mov"));
        CHECK_EQ(l.Operands, std::string("r0, #1"));
        l = arm(0x02000000, 0xE1A0C00D);
        CHECK_EQ(l.Mnemonic, std::string("mov"));
        CHECK_EQ(l.Operands, std::string("r12, sp"));
    }
    else
    {
        CHECK_EQ(l.Mnemonic, std::string(".word"));
        CHECK_EQ(l.Operands, std::string("0xe3a00001"));
    }

    l = dis.decode(0x02000000, b, 3, false);
    CHECK_EQ(l.Mnemonic, std::string("??"));
    CHECK_EQ(l.Size, (u8)4);
}

TEST(DisasmThumb)
{
    Disassembler dis;
    u8 b[4];
    auto th16 = [&](u32 pc, u16 op) { memcpy(b, &op, 2); return dis.decode(pc, b, 2, true); };

    DisasmLine l = th16(0x02000000, 0x2001);
    CHECK_EQ(l.Size, (u8)2);
    CHECK(!l.HasTarget);
    if (Disassembler::available())
    {
        CHECK_EQ(l.Mnemonic, std::string("movs"));
        CHECK_EQ(l.Operands, std::string("r0, #1"));
    }
    else
        CHECK_EQ(l.Operands, std::string("0x2001"));

    l = th16(0x02000000, 0xD0FE);
    CHECK(l.HasTarget && !l.IsCall);
    CHECK_EQ(l.Target, 0x02000000u);
    l = th16(0x02000000, 0xE7FE);
    CHECK(l.HasTarget);
    CHECK_EQ(l.Target, 0x02000000u);
    l = th16(0x02000002, 0x4801);
    CHECK(l.HasTarget && l.IsLiteral);
    CHECK_EQ(l.Target, 0x02000008u);

    // 32-bit BL / BLX pair
    u16 pair[2] = {0xF000, 0xF801};
    memcpy(b, pair, 4);
    l = dis.decode(0x02000000, b, 4, true);
    CHECK_EQ(l.Size, (u8)4);
    CHECK_EQ(l.Opcode, 0xF801F000u);
    CHECK(l.HasTarget && l.IsCall);
    CHECK_EQ(l.Target, 0x02000006u);
    CHECK_EQ(l.Mnemonic, std::string("bl"));
    CHECK_EQ(l.Operands, std::string("#0x02000006"));
    pair[1] = 0xE801;
    memcpy(b, pair, 4);
    l = dis.decode(0x02000000, b, 4, true);
    CHECK_EQ(l.Mnemonic, std::string("blx"));
    CHECK_EQ(l.Target, 0x02000004u);
    CHECK_EQ(l.Operands, std::string("#0x02000004"));

    // lone prefix/suffix halves with only two bytes available
    l = th16(0x02000000, 0xF000);
    CHECK_EQ(l.Size, (u8)2);
    CHECK_EQ(l.Mnemonic, std::string("bl.hi"));
    CHECK_EQ(l.Operands, std::string("#0x0"));
    l = th16(0x02000000, 0xF8A5);
    CHECK_EQ(l.Mnemonic, std::string("bl.lo"));
    CHECK_EQ(l.Operands, std::string("#0xa5"));

    l = dis.decode(0x02000000, b, 1, true);
    CHECK_EQ(l.Mnemonic, std::string("??"));
    CHECK_EQ(l.Size, (u8)2);
}

// ---- ROM overlays (the core's RomOverlays.cpp, which links without the emulator) and DWARF line info

namespace
{

void Put32(std::string& buf, size_t at, u32 v)
{
    if (buf.size() < at + 4)
        buf.resize(at + 4, '\0');
    memcpy(&buf[at], &v, 4);
}

// "ABC" * 6 as BLZ: three literals from the end, then a 15-byte copy at distance 3.
std::vector<u8> BlzSample()
{
    std::vector<u8> v = {0x00, 0xC0, 'A', 'B', 'C', 0x10};
    u32 footer = (8u << 24) | 14u, extra = 4;
    v.resize(14);
    memcpy(&v[6], &footer, 4);
    memcpy(&v[10], &extra, 4);
    return v;
}

}

TEST(BlzImages)
{
    std::vector<u8> data = BlzSample();
    CHECK(BlzDecompress(data));
    CHECK_EQ(std::string(data.begin(), data.end()), std::string("ABCABCABCABCABCABC"));

    // extra = 0: stored uncompressed, left alone
    std::vector<u8> plain = {1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(BlzDecompress(plain));
    CHECK_EQ(plain.size(), (size_t)12);

    std::vector<u8> tiny = {1, 2, 3};
    CHECK(!BlzDecompress(tiny));
    std::vector<u8> bad = BlzSample();
    bad[6] = 0xFF; // packed size larger than the file
    CHECK(!BlzDecompress(bad));
}

TEST(RomOverlayTable)
{
    // a ROM with an overlay table of two entries: ov 3 stored plain, ov 7 BLZ-compressed
    std::string rom(0x400, '\0');
    NDSHeader h {};
    h.ARM9OverlayOffset = 0x100;
    h.ARM9OverlaySize = 64;
    h.FATOffset = 0x200;
    std::string plainImage;
    for (int i = 0; i < 0x40; i++)
        plainImage += (char)(i * 5 + 1);
    rom.replace(0x300, plainImage.size(), plainImage);
    std::vector<u8> packed = BlzSample();
    rom.replace(0x380, packed.size(), std::string(packed.begin(), packed.end()));

    // entry: id, ram address, ram size, bss size, sinit start, sinit end, file id, flags
    Put32(rom, 0x100, 3); Put32(rom, 0x104, 0x02200000); Put32(rom, 0x108, 0x40); Put32(rom, 0x10C, 0x10);
    Put32(rom, 0x118, 0); Put32(rom, 0x11C, 0);
    Put32(rom, 0x120, 7); Put32(rom, 0x124, 0x02200000); Put32(rom, 0x128, 18); Put32(rom, 0x12C, 2);
    Put32(rom, 0x138, 1); Put32(rom, 0x13C, (1u << 24) | (u32)packed.size());
    // FAT: file 0 at 0x300-0x340, file 1 at 0x380-0x38E
    Put32(rom, 0x200, 0x300); Put32(rom, 0x204, 0x340);
    Put32(rom, 0x208, 0x380); Put32(rom, 0x20C, 0x380 + (u32)packed.size());

    auto ovs = ReadRomOverlays((const u8*)rom.data(), (u32)rom.size(), h);
    CHECK_EQ(ovs.size(), (size_t)2);
    if (ovs.size() != 2)
        return;
    CHECK_EQ(ovs[0].Id, 3);
    CHECK_EQ(ovs[0].Base, 0x02200000u);
    CHECK_EQ(ovs[0].Size, 0x50u);
    CHECK_EQ(std::string(ovs[0].Image.begin(), ovs[0].Image.end()), plainImage);
    CHECK_EQ(ovs[1].Id, 7);
    CHECK_EQ(ovs[1].Size, 20u);
    CHECK_EQ(std::string(ovs[1].Image.begin(), ovs[1].Image.end()), std::string("ABCABCABCABCABCABC"));

    // the table size counts the entries; no ROM: none
    h.ARM9OverlaySize = 32;
    CHECK_EQ(ReadRomOverlays((const u8*)rom.data(), (u32)rom.size(), h).size(), (size_t)1);
    CHECK(ReadRomOverlays(nullptr, 0, h).empty());

    // detection: ov 3 resident in "memory", then overwritten by ov 7
    std::string ram(0x100, '\0');
    auto peek = [&ram](u32 addr) -> u32 {
        u32 v = 0;
        if (addr >= 0x02200000 && addr - 0x02200000 + 4 <= ram.size())
            memcpy(&v, &ram[addr - 0x02200000], 4);
        return v;
    };
    CHECK(!OverlayLoaded(peek, ovs[0].Base, ovs[0].Size, ovs[0].Image));
    ram.replace(0, plainImage.size(), plainImage);
    CHECK(OverlayLoaded(peek, ovs[0].Base, ovs[0].Size, ovs[0].Image));
    CHECK(!OverlayLoaded(peek, ovs[0].Base, ovs[0].Size, {}));
    ram.assign(0x100, '\0');
    ram.replace(0, 18, std::string(ovs[1].Image.begin(), ovs[1].Image.end()));
    CHECK(!OverlayLoaded(peek, ovs[0].Base, ovs[0].Size, ovs[0].Image));
    CHECK(OverlayLoaded(peek, ovs[1].Base, ovs[1].Size, ovs[1].Image));

    // the symbol table's detection is the core's
    DebugSymbols::Set set;
    DebugSymbols::Module mod;
    mod.Overlay = 7;
    mod.Base = ovs[1].Base;
    mod.Size = ovs[1].Size;
    mod.Image = ovs[1].Image;
    set.Modules.push_back(mod);
    CHECK(DebugSymbols::detect(set, peek) == std::vector<int>({7}));
}

namespace
{

// A 32-bit little-endian ELF with only .shstrtab and the given .debug_line.
std::string MakeElf(const std::string& debugLine)
{
    std::string names = std::string("\0.shstrtab\0.debug_line\0", 23);
    std::string elf(0x34, '\0');
    memcpy(&elf[0], "\x7F" "ELF", 4);
    elf[4] = 1; // 32-bit
    elf[5] = 1; // little-endian
    elf[6] = 1;
    u32 namesAt = (u32)elf.size();
    elf += names;
    u32 lineAt = (u32)elf.size();
    elf += debugLine;
    while (elf.size() % 4)
        elf += '\0';
    u32 shoff = (u32)elf.size();
    elf.resize(shoff + 3 * 40, '\0');
    auto sh = [&](int i, u32 name, u32 off, u32 size) {
        Put32(elf, shoff + i * 40 + 0, name);
        Put32(elf, shoff + i * 40 + 16, off);
        Put32(elf, shoff + i * 40 + 20, size);
    };
    sh(1, 1, namesAt, (u32)names.size());
    sh(2, 11, lineAt, (u32)debugLine.size());
    Put32(elf, 0x20, shoff);
    u16 half[3] = {40, 3, 1}; // shentsize, shnum, shstrndx
    memcpy(&elf[0x2E], half, 6);
    return elf;
}

// One DWARF 2 line program for build/../src/game.c: line 10 at addr, line 12 at addr + 8, end at addr + 16.
std::string LineProgram(u32 addr)
{
    std::string hdr;
    hdr += '\x01';           // minimum instruction length
    hdr += '\x01';           // default is_stmt
    hdr += (char)-5;         // line base
    hdr += '\x0E';           // line range
    hdr += '\x0D';           // opcode base
    hdr += std::string("\x00\x01\x01\x01\x01\x00\x00\x00\x01\x00\x00\x01", 12);
    hdr += std::string("C:/decomp/src\0\0", 15);
    hdr += std::string("game.c\0\x01\x00\x00\0", 11);
    std::string prog;
    prog += std::string("\x00\x05\x02", 3);
    prog.append((const char*)&addr, 4);
    prog += "\x03\x09\x01";  // advance_line 9 -> 10, copy
    prog += "\x02\x08\x03\x02\x01"; // advance_pc 8, advance_line 2 -> 12, copy
    prog += "\x02\x08";      // advance_pc 8
    prog += std::string("\x00\x01\x01", 3); // end_sequence
    std::string unit;
    u16 version = 2;
    unit.append((const char*)&version, 2);
    u32 headerLength = (u32)hdr.size();
    unit.append((const char*)&headerLength, 4);
    unit += hdr;
    unit += prog;
    u32 unitLength = (u32)unit.size();
    return std::string((const char*)&unitLength, 4) + unit;
}

void WriteDecomp(const TempDir& dir, bool withElf)
{
    dir.write("decomp/config/eur/arm9/config.yaml",
        "main_module:\n"
        "  name: main\n"
        "  symbols: symbols.txt\n"
        "  delinks: delinks.txt\n"
        "overlays:\n"
        "  - id: 2\n"
        "    name: ov002\n"
        "    symbols: overlays/ov002/symbols.txt\n"
        "    delinks: overlays/ov002/delinks.txt\n");
    dir.write("decomp/config/eur/arm9/symbols.txt", "fn_main kind:function(size=0x10) addr:0x02000100\n");
    dir.write("decomp/config/eur/arm9/delinks.txt", "src/main.c:\n    .text start:0x02000100 end:0x02000110\n");
    dir.write("decomp/config/eur/arm9/overlays/ov002/symbols.txt", "fn_game kind:function(size=0x10) addr:0x02200000\n");
    dir.write("decomp/config/eur/arm9/overlays/ov002/delinks.txt", "src/game.c:\n    .text start:0x02200000 end:0x02200010\n");
    if (withElf)
        dir.write("decomp/build/eur/arm9.o", MakeElf(LineProgram(0x02200000)));
}

}

TEST(DwarfLineInfo)
{
    TempDir dir("lines");
    WriteDecomp(dir, true);
    DebugSymbols syms;
    std::string err;
    // the decomp's build/<region>/arm9.o loads with its config
    CHECK(syms.load(dir.file("decomp/config/eur/arm9/config.yaml"), err));
    auto set = syms.set();
    CHECK(set != nullptr);
    if (!set)
        return;
    CHECK_EQ(syms.lineSource(), dir.file("decomp/build/eur/arm9.o"));
    CHECK_EQ(set->LineCount, 2);
    CHECK(set->LineFiles == std::vector<std::string>({"src/game.c"}));
    CHECK_EQ(set->Modules[1].Lines.size(), (size_t)3);

    // overlay lines resolve only while it is loaded
    std::string file;
    int line = 0;
    u32 addr = 0;
    int module = -1;
    CHECK(!syms.lineAt(0x02200004, file, line));
    CHECK(!syms.lineAddress("src/game.c", 12, addr, module));
    CHECK_EQ(module, 1);
    syms.setLoaded({2});
    CHECK(syms.lineAt(0x02200004, file, line) && file == "src/game.c" && line == 10);
    CHECK(syms.lineAt(0x02200009, file, line) && line == 12);
    CHECK(!syms.lineAt(0x02200010, file, line)); // past the end of the sequence
    CHECK(syms.lineAddress("src/game.c", 12, addr, module) && addr == 0x02200008 && module == 1);
    CHECK(!syms.lineAddress("src/game.c", 11, addr, module));
    CHECK(!syms.lineAddress("src/other.c", 10, addr, module));

    // labels keep the line info; a new load drops it
    dir.write("labels.txt", "02200000 game_entry ov002\n");
    CHECK(syms.loadLabels(dir.file("labels.txt"), err));
    CHECK(syms.lineAt(0x02200004, file, line) && line == 10);
    CHECK_EQ(syms.describe(0x02200000), std::string("game_entry [ov002]"));

    TempDir bare("lines_bare");
    WriteDecomp(bare, false);
    DebugSymbols other;
    CHECK(other.load(bare.file("decomp/config/eur/arm9/config.yaml"), err));
    CHECK(other.lineSource().empty());
    CHECK(!other.loadLines(bare.file("decomp/build/eur/arm9.o"), err));
    CHECK_EQ(err, "not found: " + bare.file("decomp/build/eur/arm9.o"));
    bare.write("noline.o", MakeElf(""));
    CHECK(!other.loadLines(bare.file("noline.o"), err));
    // any ELF can be loaded by hand
    CHECK(other.loadLines(dir.file("decomp/build/eur/arm9.o"), err));
    CHECK_EQ(other.set()->LineCount, 2);

    DebugSymbols none;
    CHECK(!none.loadLines(dir.file("decomp/build/eur/arm9.o"), err));
    CHECK_EQ(err, std::string("load symbols first"));
}

TEST_MAIN()
