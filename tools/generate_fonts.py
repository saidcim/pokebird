#!/usr/bin/env python3
"""
generate_fonts.py — build the UI's LVGL fonts (firmware/src/ui/fonts/).

WHY THIS EXISTS: LVGL's bundled Montserrat has no accented Latin letters
(ç ğ ı İ ö ş ü), so they render as boxes. Species names on screen are English
now, but the scientific names and the design's separator glyphs still need
coverage beyond plain ASCII, and the accented set is kept so the fonts stay
usable if a localised build is ever wanted.

MATCHING THE DESIGN: the design uses Oswald (a condensed grotesque) and Space
Mono; both are Google Fonts and neither is on the machine. NOTHING IS
DOWNLOADED — two of Windows' own fonts that fill the same roles were chosen
instead:

    Oswald      -> Liberation Sans Narrow Bold   (condensed grotesque, Latin Ext-A)
    Space Mono  -> DejaVu Sans Mono              (the oblique cut, for scientific names)

Both cover the accented set fully, and this script VERIFIES that after
generating (it exits with an error on a missing glyph rather than silently
letting a box reach the screen).

Usage:
    python tools/generate_fonts.py

Requires: Node/npm (lv_font_conv is fetched with npx; no install needed).
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "firmware" / "src" / "ui" / "fonts"
WINFONT = Path("C:/Windows/Fonts")

# ── Accented glyphs ──────────────────────────────────────────────────────────
# Every code point needed IN ADDITION to ASCII. Each one is looked up after
# generation: if any is dropped, the screen shows a box, and catching that by
# eye is expensive.
ACCENTED = "ÇçĞğİıÖöŞşÜüÂâÎîÛû"
# The separator used in headings, plus the degree sign.
EXTRA = "·°"

SYMBOLS = ACCENTED + EXTRA

# ── Fonts to generate ────────────────────────────────────────────────────────
# The sizes come from the design's px values (the screen is 640x172, so the
# design's px IS the device's px — there is no scaling):
#   species name 17px · heading/percentage 13-14px · scientific name 10px
FONTS = [
    # (output name,        ttf,                                size, bpp)
    ("pb_font_name_18",    "LiberationSansNarrow-Bold.ttf",      18, 4),
    ("pb_font_bold_13",    "LiberationSansNarrow-Bold.ttf",      13, 4),
    ("pb_font_narrow_11",  "LiberationSansNarrow-Regular.ttf",   11, 4),
    ("pb_font_mono_10",    "DejaVuSansMono-Oblique.ttf",         10, 4),
]


def npx_command() -> list[str]:
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if not npx:
        sys.exit("ERROR: npx not found. Node.js must be installed.")
    return [npx, "--yes", "lv_font_conv@latest"]


def generate(name: str, ttf: str, size: int, bpp: int) -> Path:
    source = WINFONT / ttf
    if not source.is_file():
        sys.exit(f"ERROR: font not found: {source}")

    target = OUTPUT / f"{name}.c"
    cmd = npx_command() + [
        "--font", str(source),
        "-r", "0x20-0x7E",
        "--symbols", SYMBOLS,
        "--size", str(size),
        "--bpp", str(bpp),
        # Compression is deliberately OFF: the card is redrawn slice by slice,
        # so the decode cost would be paid again on every slice. Flash is
        # plentiful here (16 MB); CPU is not.
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
        sys.exit(f"ERROR: could not generate {name}")
    return target


def verify(path: Path, name: str) -> None:
    """Check that every accented code point is REALLY in the generated .c.

    lv_font_conv drops a missing glyph silently, and seeing the result as a box
    on screen costs a whole round of looking. This reads the source file
    instead: the glyph ranges are written in the `.range_start` /
    `.range_length` fields.
    """
    text = path.read_text(encoding="utf-8", errors="replace")

    ranges: list[tuple[int, int]] = []
    for m in re.finditer(r"\.range_start\s*=\s*(\d+).*?\.range_length\s*=\s*(\d+)",
                         text, re.S):
        start, length = int(m.group(1)), int(m.group(2))
        ranges.append((start, start + length))

    if not ranges:
        sys.exit(f"ERROR: no glyph ranges found in {name} "
                 f"(the format may have changed)")

    missing = [c for c in SYMBOLS
               if not any(a <= ord(c) < b for a, b in ranges)]
    if missing:
        sys.exit(f"ERROR: glyphs missing from {name}: {''.join(missing)}")


def main() -> int:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    print(f"Fonts -> {OUTPUT.relative_to(ROOT)}")
    print(f"Accented glyphs: {ACCENTED}")

    for name, ttf, size, bpp in FONTS:
        path = generate(name, ttf, size, bpp)
        verify(path, name)
        print(f"  {'':<18} -> {path.stat().st_size / 1024:.1f} KB  "
              f"(full coverage)")

    print("\nAll fonts generated and their glyph coverage verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
