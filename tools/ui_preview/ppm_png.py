#!/usr/bin/env python3
"""
ppm_png.py — converts the PPMs ui_preview leaves behind into PNGs.

Pure stdlib (zlib + struct); Pillow is not required. Each image is also saved
SCALED UP: the screen is 640x172, and at 1:1 the font detail is hard to make
out.

    python tools/ui_preview/ppm_png.py
"""
from __future__ import annotations

import pathlib
import struct
import sys
import zlib

BURA = pathlib.Path(__file__).resolve().parent
SCALE = 2


def ppm_oku(path: pathlib.Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
    # Header: P6\n<w> <h>\n255\n — parse the three fields in order.
    alanlar, i = [], 0
    while len(alanlar) < 4:
        while i < len(raw) and raw[i : i + 1].isspace():
            i += 1
        j = i
        while j < len(raw) and not raw[j : j + 1].isspace():
            j += 1
        alanlar.append(raw[i:j].decode())
        i = j
    i += 1  # the single whitespace separator
    w, h = int(alanlar[1]), int(alanlar[2])
    return w, h, raw[i : i + w * h * 3]


def png_write(path: pathlib.Path, w: int, h: int, rgb: bytes, scale: int = 1) -> None:
    if scale > 1:
        buyuk = bytearray()
        for y in range(h):
            row = bytearray()
            for x in range(w):
                p = rgb[(y * w + x) * 3 : (y * w + x) * 3 + 3]
                row += p * scale
            buyuk += row * scale
        rgb, w, h = bytes(buyuk), w * scale, h * scale

    # A filter byte at the start of each row (0 = None).
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3 : (y + 1) * w * 3]

    def parca(tip: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tip + data
                + struct.pack(">I", zlib.crc32(tip + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += parca(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += parca(b"IDAT", zlib.compress(bytes(raw), 9))
    png += parca(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    ppmler = sorted(BURA.glob("build/*.ppm")) or sorted(BURA.glob("*.ppm"))
    if not ppmler:
        sys.exit("PPM yok — once ui_preview calistirin.")

    for p in ppmler:
        w, h, rgb = ppm_oku(p)
        target = p.with_suffix(".png")
        png_write(target, w, h, rgb, SCALE)
        print(f"  {target.name}  {w*SCALE}x{h*SCALE}")
        p.unlink()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
