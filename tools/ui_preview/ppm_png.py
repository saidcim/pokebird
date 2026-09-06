#!/usr/bin/env python3
"""
ppm_png.py — ui_preview'nin bıraktığı PPM'leri PNG'ye çevirir.

Saf stdlib (zlib + struct); Pillow gerekmiyor. Ayrıca her görüntüyü BÜYÜTÜP
kaydediyor: ekran 640x172 ve 1:1 bakıldığında yazı tipi ayrıntısı seçilmiyor.

    python tools/ui_preview/ppm_png.py
"""
from __future__ import annotations

import pathlib
import struct
import sys
import zlib

BURA = pathlib.Path(__file__).resolve().parent
OLCEK = 2


def ppm_oku(yol: pathlib.Path) -> tuple[int, int, bytes]:
    ham = yol.read_bytes()
    # Başlık: P6\n<w> <h>\n255\n — üç alanı sırayla ayıkla.
    alanlar, i = [], 0
    while len(alanlar) < 4:
        while i < len(ham) and ham[i : i + 1].isspace():
            i += 1
        j = i
        while j < len(ham) and not ham[j : j + 1].isspace():
            j += 1
        alanlar.append(ham[i:j].decode())
        i = j
    i += 1  # tek boşluk ayracı
    w, h = int(alanlar[1]), int(alanlar[2])
    return w, h, ham[i : i + w * h * 3]


def png_yaz(yol: pathlib.Path, w: int, h: int, rgb: bytes, olcek: int = 1) -> None:
    if olcek > 1:
        buyuk = bytearray()
        for y in range(h):
            satir = bytearray()
            for x in range(w):
                p = rgb[(y * w + x) * 3 : (y * w + x) * 3 + 3]
                satir += p * olcek
            buyuk += satir * olcek
        rgb, w, h = bytes(buyuk), w * olcek, h * olcek

    # Her satırın başına filtre baytı (0 = None).
    ham = bytearray()
    for y in range(h):
        ham.append(0)
        ham += rgb[y * w * 3 : (y + 1) * w * 3]

    def parca(tip: bytes, veri: bytes) -> bytes:
        return (struct.pack(">I", len(veri)) + tip + veri
                + struct.pack(">I", zlib.crc32(tip + veri) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += parca(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += parca(b"IDAT", zlib.compress(bytes(ham), 9))
    png += parca(b"IEND", b"")
    yol.write_bytes(png)


def main() -> int:
    ppmler = sorted(BURA.glob("build/*.ppm")) or sorted(BURA.glob("*.ppm"))
    if not ppmler:
        sys.exit("PPM yok — once ui_preview calistirin.")

    for p in ppmler:
        w, h, rgb = ppm_oku(p)
        hedef = p.with_suffix(".png")
        png_yaz(hedef, w, h, rgb, OLCEK)
        print(f"  {hedef.name}  {w*OLCEK}x{h*OLCEK}")
        p.unlink()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
