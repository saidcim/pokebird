/**
 * text.h — UTF-8 aware text helpers for the UI
 *
 * The design shows species names in UPPER CASE (Oswald, uppercase). C's
 * toupper() is byte-oriented, so it mangles any multi-byte sequence it is
 * handed. This helper uppercases ASCII correctly, maps the accented Latin
 * letters the fonts carry, and — the part that actually matters — never
 * truncates in the middle of a UTF-8 sequence.
 *
 * No hardware dependencies (stdint/stddef only); covered by the host tests.
 */
#ifndef POKEBIRD_TEXT_H
#define POKEBIRD_TEXT_H

#include <stdint.h>

/**
 * Uppercase a UTF-8 string into `out`, writing at most `n` bytes.
 *
 * Coverage: ASCII a-z, plus the accented letters àâçèéêîôöûü and their
 * relatives that the generated fonts include. Unrecognised multi-byte
 * sequences are copied THROUGH UNCHANGED — leaving a character alone beats
 * corrupting it.
 *
 * The output is always NUL-terminated. If the input does not fit it is cut
 * short, and the cut never lands inside a UTF-8 sequence.
 *
 * NOTE ON TURKISH: an earlier version of this function implemented Turkish
 * casing, where `i` uppercases to the dotted `İ`. That is correct Turkish and
 * wrong for the English species names the device now displays — it rendered
 * "Common Nightingale" as "COMMON NİGHTİNGALE". ASCII `i` now maps to `I`.
 * The dotless `ı` still maps to `I`, which is right in both languages.
 */
void pb_text_upper(const char *utf8, char *out, uint32_t n);

#endif /* POKEBIRD_TEXT_H */
