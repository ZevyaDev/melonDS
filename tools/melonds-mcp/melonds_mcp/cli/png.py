"""A minimal PNG encoder (zlib + struct; no imaging library)."""
import struct
import zlib


def _chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def encode_bgra(pixels, width, height, alpha=False):
    """pixels: BGRA bytes, width*height*4. Returns PNG bytes (RGB, or RGBA with alpha=True), filter 0."""
    if len(pixels) != width * height * 4:
        raise ValueError("pixel buffer does not match the size")
    rows = []
    for y in range(height):
        row = bytearray(pixels[y * width * 4:(y + 1) * width * 4])
        row[0::4], row[2::4] = row[2::4], row[0::4]            # BGRA -> RGBA
        if not alpha:
            del row[3::4]
        rows.append(b"\x00" + bytes(row))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
            + _chunk(b"IEND", b""))
