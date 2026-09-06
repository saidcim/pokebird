#include "ui/text.h"

/* Two-byte lowercase letters the generated fonts carry, and their uppercase
 * forms. Anything not in this table is copied through untouched. */
typedef struct { uint16_t lower, upper; } case_pair_t;

static const case_pair_t TWO_BYTE[] = {
    { 0xC3A7, 0xC387 },   /* ç -> Ç */
    { 0xC3B6, 0xC396 },   /* ö -> Ö */
    { 0xC3BC, 0xC39C },   /* ü -> Ü */
    { 0xC49F, 0xC49E },   /* ğ -> Ğ */
    { 0xC59F, 0xC59E },   /* ş -> Ş */
    { 0xC3A2, 0xC382 },   /* â -> Â */
    { 0xC3AE, 0xC38E },   /* î -> Î */
    { 0xC3BB, 0xC39B },   /* û -> Û */
};

void pb_text_upper(const char *utf8, char *out, uint32_t n)
{
    if (!out || n == 0) return;
    if (!utf8) { out[0] = '\0'; return; }

    uint32_t j = 0;
    const unsigned char *p = (const unsigned char *)utf8;

    while (*p) {
        /* ── Single byte (ASCII) ── */
        if (*p < 0x80) {
            if (j + 1 >= n) break;
            char c = (char)*p++;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[j++] = c;
            continue;
        }

        /* ── Multi-byte: read the length from the lead byte ── */
        uint32_t len = (*p & 0xE0) == 0xC0 ? 2
                     : (*p & 0xF0) == 0xE0 ? 3
                     : (*p & 0xF8) == 0xF0 ? 4 : 1;

        /* ı (C4 B1) -> I : two bytes collapse to one, so handle it apart. */
        if (len == 2 && p[0] == 0xC4 && p[1] == 0xB1) {
            if (j + 1 >= n) break;
            out[j++] = 'I';
            p += 2;
            continue;
        }

        if (len == 2) {
            const uint16_t two = (uint16_t)((p[0] << 8) | p[1]);
            uint16_t write = two;
            for (uint32_t i = 0; i < sizeof(TWO_BYTE) / sizeof(TWO_BYTE[0]); i++) {
                if (TWO_BYTE[i].lower == two) { write = TWO_BYTE[i].upper; break; }
            }
            if (j + 2 >= n) break;
            out[j++] = (char)(write >> 8);
            out[j++] = (char)(write & 0xFF);
            p += 2;
            continue;
        }

        /* Unrecognised sequence: copy as-is — but never cut it in half. */
        if (j + len >= n) break;
        for (uint32_t i = 0; i < len && p[i]; i++) out[j++] = (char)p[i];
        p += len;
    }

    out[j] = '\0';
}
