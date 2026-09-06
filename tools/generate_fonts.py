#!/usr/bin/env python3
"""
font_uret.py — Arayüzün LVGL yazı tiplerini üretir (src/ui/fonts/).

NEDEN VAR: LVGL'in gömülü Montserrat'ında Türkçe harfler (ç ğ ı İ ö ş ü) YOK;
yazılırsa kutu çıkıyor. §9p bu yüzden tür adlarını ASCII'ye indiriyordu
("Ak Karınlı Ebabil" -> "Ak Karinli Ebabil"). Burada üretilen yazı tipleri o
borcu kapatıyor: adlar artık ekranda TAM TÜRKÇE yazılıyor.

TASARIMA UYUM: Kus Sesi Arayuz.dc.html Oswald (sıkışık grotesk) + Space Mono
kullanıyor; ikisi de Google Fonts, makinede yok. İNDİRME YAPILMIYOR — Windows'un
kendi yazı tiplerinden aynı role oturan ikisi seçildi:

    Oswald      -> Liberation Sans Narrow Bold   (sıkışık grotesk, Latin Ext-A)
    Space Mono  -> DejaVu Sans Mono              (eğik hâli bilimsel adlar için)

Her ikisinin de Türkçe kapsaması tam; script üretimden sonra bunu DOĞRULUYOR
(eksik glif varsa hata verip çıkıyor, sessizce kutu basmıyor).

Kullanım:
    python tools/generate_fonts.py

Gereken: Node/npm (lv_font_conv npx ile çekiliyor, kurulum gerekmiyor).
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "src" / "ui" / "fonts"
WINFONT = Path("C:/Windows/Fonts")

# ── Türkçe glifler ────────────────────────────────────────────────────────────
# ASCII'ye EK olarak gereken her kod noktası. Bunlar üretimden sonra tek tek
# aranıyor: biri düşerse ekranda kutu çıkar ve bunu gözle yakalamak pahalı.
TURKCE = "ÇçĞğİıÖöŞşÜüÂâÎîÛû"
# "BUGÜN · TODAY" başlığındaki ayraç ve yüzde/derece işaretleri.
EKSTRA = "·°"

SIMGELER = TURKCE + EKSTRA

# ── Üretilecek yazı tipleri ───────────────────────────────────────────────────
# Boyutlar tasarımdaki px değerlerinden geliyor (ekran 640x172, yani tasarımın
# px'i CİHAZIN px'i — ölçek yok):
#   tür adı 17px · başlık/yüzde 13-14px · bilimsel ad 10px
FONTLAR = [
    # (çıktı adı,           ttf,                          boyut, bpp)
    ("pb_font_name_18",       "LiberationSansNarrow-Bold.ttf",  18, 4),
    ("pb_font_bold_13",    "LiberationSansNarrow-Bold.ttf",  13, 4),
    ("pb_font_narrow_11",      "LiberationSansNarrow-Regular.ttf", 11, 4),
    ("pb_font_mono_10",     "DejaVuSansMono-Oblique.ttf",     10, 4),
]


def npx_komutu() -> list[str]:
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if not npx:
        sys.exit("HATA: npx bulunamadi. Node.js kurulu olmali.")
    return [npx, "--yes", "lv_font_conv@latest"]


def uret(name: str, ttf: str, size: int, bpp: int) -> Path:
    source = WINFONT / ttf
    if not source.is_file():
        sys.exit(f"HATA: yazi tipi yok: {source}")

    target = OUTPUT / f"{name}.c"
    cmd = npx_komutu() + [
        "--font", str(source),
        "-r", "0x20-0x7E",
        "--symbols", SIMGELER,
        "--size", str(size),
        "--bpp", str(bpp),
        # Sıkıştırma AÇILMIYOR: kart artık dilim dilim yeniden çiziliyor, çözme
        # maliyeti her dilimde tekrar ödenirdi. Flash bizde bol (16 MB), CPU değil.
        "--no-compress",
        "--format", "lvgl",
        "--force-fast-kern-format",
        "--lv-include", "lvgl.h",
        "-o", str(target),
    ]
    print(f"  {name:<18} {ttf} {size}px bpp{bpp}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        sys.exit(f"HATA: {name} uretilemedi")
    return target


def verify(path: Path, name: str) -> None:
    """Üretilen .c dosyasında her Türkçe kod noktası GERÇEKTEN var mı.

    lv_font_conv eksik glifi sessizce atlıyor; sonucu ekranda kutu olarak
    görmek bir tur göz demek. Burada kaynak dosyadan okunuyor: glif aralıkları
    `.range_start` / `.range_length` alanlarında yazılı.
    """
    text = path.read_text(encoding="utf-8", errors="replace")

    araliklar: list[tuple[int, int]] = []
    for m in re.finditer(r"\.range_start\s*=\s*(\d+).*?\.range_length\s*=\s*(\d+)",
                         text, re.S):
        basla, length = int(m.group(1)), int(m.group(2))
        araliklar.append((basla, basla + length))

    if not araliklar:
        sys.exit(f"HATA: {name} icinde glif araligi bulunamadi (bicim degismis olabilir)")

    missing = [c for c in SIMGELER
             if not any(a <= ord(c) < b for a, b in araliklar)]
    if missing:
        sys.exit(f"HATA: {name} icinde eksik glif: {''.join(missing)}")


def main() -> int:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    print(f"Yazi tipleri -> {OUTPUT.relative_to(ROOT)}")
    print(f"Turkce glifler: {TURKCE}")

    for name, ttf, size, bpp in FONTLAR:
        path = uret(name, ttf, size, bpp)
        verify(path, name)
        print(f"  {'':<18} -> {path.stat().st_size / 1024:.1f} KB  (Turkce tam)")

    print("\nHepsi uretildi ve Turkce kapsamasi dogrulandi.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
