"""memory_search and text_find over the socket."""
from melonds_mcp.client import ENC_ASCII, ENC_UTF8, ENC_UTF16LE, T

from .base import DATA
from .server_base import ServerTest

AREA = DATA            # zero-filled test area
SPAN = 0x10000


class MemorySearchTests(ServerTest):
    def setUp(self):
        super().setUp()
        self.c.write(AREA, bytes(SPAN))
        self.cl = self.connect()

    def put(self, offset, data, cpu=9):
        self.c.write(AREA + offset, data, cpu=cpu)
        return AREA + offset

    def search(self, needle, size=0, **kw):
        return self.cl.memory_search(needle, size, start=AREA, length=SPAN, **kw)

    def test_u32_aligned_only(self):
        a = self.put(0x1000, (0xDEADBEEF).to_bytes(4, "little"))
        self.put(0x2001, (0xDEADBEEF).to_bytes(4, "little"))
        self.assertEqual(self.search(0xDEADBEEF, 4), ([a], False))

    def test_u16_aligned_only(self):
        a = self.put(0x1100, b"\x5c\x7a")
        self.put(0x1201, b"\x5c\x7a")
        self.assertEqual(self.search(0x7A5C, 2), ([a], False))

    def test_u8_anywhere(self):
        a = self.put(0x1301, b"\x5a")
        b = self.put(0x1302, b"\x5a")
        self.assertEqual(self.search(0x5A, 1), ([a, b], False))

    def test_byte_string_at_any_offset(self):
        a = self.put(0x1403, b"ZYX-needle")
        self.put(0x1500, b"ZYX-needl")
        self.assertEqual(self.search(b"ZYX-needle", 0), ([a], False))

    def test_overlapping_matches(self):
        a = self.put(0x1600, b"aaaa")
        self.assertEqual(self.search(b"aa", 0), ([a, a + 1, a + 2], False))

    def test_limit_and_truncated(self):
        addrs = [self.put(0x2000 + 8 * i, (0x11223344).to_bytes(4, "little")) for i in range(10)]
        self.assertEqual(self.search(0x11223344, 4, limit=3), (addrs[:3], True))
        self.assertEqual(self.search(0x11223344, 4, limit=10), (addrs, False))
        self.assertEqual(self.search(0x11223344, 4, limit=0), (addrs, False))

    def test_limit_clamped_to_max(self):
        found, truncated = self.cl.memory_search(0, 4, start=AREA, length=SPAN, limit=100000)
        self.assertEqual(len(found), 4096)
        self.assertTrue(truncated)

    def test_default_range_is_main_ram(self):
        a = 0x02300000 + 0x40
        self.c.write(a, b"\x01\x23\x45\x67\x89\xab\xcd\xef")
        self.assertEqual(self.cl.memory_search(b"\x01\x23\x45\x67\x89\xab\xcd\xef", 0), ([a], False))

    def test_arm7_view(self):
        a = self.put(0x3000, b"\xa5\x5a\xa5\x5a", cpu=7)
        self.assertEqual(self.cl.memory_search(0x5AA55AA5, 4, start=AREA, length=SPAN, cpu=7), ([a], False))

    def test_match_does_not_run_past_range(self):
        self.put(SPAN - 2, b"\x01\x02")
        self.c.write(AREA + SPAN, b"\x03\x04")
        self.assertEqual(self.search(b"\x01\x02\x03\x04", 0), ([], False))

    def test_invalid_requests(self):
        for size, needle in ((3, b"abc"), (4, b"ab"), (0, b"")):
            body = (bytes([9]) + (AREA).to_bytes(4, "little") + (SPAN).to_bytes(4, "little") + bytes([size])
                    + (0).to_bytes(4, "little") + len(needle).to_bytes(4, "little") + needle)
            self.expect_error("bad_payload", self.cl.request, T.memory_search, body)
        self.expect_error("bad_payload", self.cl.memory_search, b"x", 0, start=0xFFFFFF00, length=0x200)
        self.expect_error("bad_payload", self.cl.memory_search, b"x", 0, start=AREA, length=0x2000000)


class TextFindTests(ServerTest):
    def setUp(self):
        super().setUp()
        self.c.write(AREA, bytes(SPAN))
        self.cl = self.connect()

    def put(self, offset, data):
        self.c.write(AREA + offset, data)
        return AREA + offset

    def find(self, text, enc=0, **kw):
        return self.cl.text_find(text, enc, start=AREA, length=SPAN, **kw)

    def test_ascii_reported_once(self):
        a = self.put(0x100, b"HelloWorldXQ")
        self.assertEqual(self.find("HelloWorldXQ"), ([(a, ENC_ASCII)], False))

    def test_ascii_text_with_only_utf8_selected(self):
        a = self.put(0x100, b"HelloWorldXQ")
        self.assertEqual(self.find("HelloWorldXQ", ENC_UTF8), ([(a, ENC_UTF8)], False))

    def test_utf16le(self):
        a = self.put(0x200, "HelloWorldXQ".encode("utf-16le"))
        self.assertEqual(self.find("HelloWorldXQ"), ([(a, ENC_UTF16LE)], False))
        self.assertEqual(self.find("HelloWorldXQ", ENC_ASCII), ([], False))

    def test_all_encodings_sorted_by_address(self):
        b = self.put(0x300, "Zebra!".encode("utf-16le"))
        a = self.put(0x100, b"Zebra!")
        self.assertEqual(self.find("Zebra!"), ([(a, ENC_ASCII), (b, ENC_UTF16LE)], False))

    def test_non_ascii_skips_ascii(self):
        u8 = self.put(0x400, "CaféX".encode("utf-8"))
        u16 = self.put(0x500, "CaféX".encode("utf-16le"))
        self.assertEqual(self.find("CaféX"), ([(u8, ENC_UTF8), (u16, ENC_UTF16LE)], False))
        self.assertEqual(self.find("CaféX", ENC_ASCII), ([], False))

    def test_limit_and_truncated(self):
        addrs = [self.put(0x1000 + 0x20 * i, b"Needle") for i in range(5)]
        found, truncated = self.find("Needle", limit=2)
        self.assertEqual(([a for a, _ in found], truncated), (addrs[:2], True))

    def test_default_range_is_main_ram(self):
        a = 0x02310000
        self.c.write(a, b"UniqueTextHere123")
        found, _ = self.cl.text_find("UniqueTextHere123")
        self.assertEqual(found, [(a, ENC_ASCII)])

    def test_empty_text_rejected(self):
        self.expect_error("bad_payload", self.cl.text_find, "")
