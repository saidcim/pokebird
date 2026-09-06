#include "ui/metin.h"

/* Türkçe'de büyütmenin ASCII'den ayrıldığı iki yer:
 *     i (U+0069)  ->  İ (U+0130, C4 B0)
 *     ı (U+0131)  ->  I (U+0049)
 * Geri kalanı olağan; iki baytlık küçük harfler tablo ile eşleniyor. */
typedef struct { uint16_t kucuk, buyuk; } eslesme_t;

static const eslesme_t IKI_BAYT[] = {
    { 0xC3A7, 0xC387 },   /* ç -> Ç */
    { 0xC3B6, 0xC396 },   /* ö -> Ö */
    { 0xC3BC, 0xC39C },   /* ü -> Ü */
    { 0xC49F, 0xC49E },   /* ğ -> Ğ */
    { 0xC59F, 0xC59E },   /* ş -> Ş */
    { 0xC3A2, 0xC382 },   /* â -> Â */
    { 0xC3AE, 0xC38E },   /* î -> Î */
    { 0xC3BB, 0xC39B },   /* û -> Û */
};

void pb_turkce_buyut(const char *utf8, char *out, uint32_t n)
{
    if (!out || n == 0) return;
    if (!utf8) { out[0] = '\0'; return; }

    uint32_t j = 0;
    const unsigned char *p = (const unsigned char *)utf8;

    while (*p) {
        /* ── Tek baytlık (ASCII) ── */
        if (*p < 0x80) {
            if (j + 1 >= n) break;
            char c = (char)*p++;
            if (c == 'i') {
                /* i -> İ, iki bayt: sığmıyorsa hiç yazma. */
                if (j + 2 >= n) break;
                out[j++] = (char)0xC4;
                out[j++] = (char)0xB0;
                continue;
            }
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[j++] = c;
            continue;
        }

        /* ── Çok baytlık: uzunluğu başlangıç baytından oku ── */
        uint32_t uzunluk = (*p & 0xE0) == 0xC0 ? 2
                         : (*p & 0xF0) == 0xE0 ? 3
                         : (*p & 0xF8) == 0xF0 ? 4 : 1;

        /* ı (C4 B1) -> I : iki bayt tek bayta iniyor, ayrı ele alınıyor. */
        if (uzunluk == 2 && p[0] == 0xC4 && p[1] == 0xB1) {
            if (j + 1 >= n) break;
            out[j++] = 'I';
            p += 2;
            continue;
        }

        if (uzunluk == 2) {
            const uint16_t iki = (uint16_t)((p[0] << 8) | p[1]);
            uint16_t yaz = iki;
            for (uint32_t i = 0; i < sizeof(IKI_BAYT) / sizeof(IKI_BAYT[0]); i++) {
                if (IKI_BAYT[i].kucuk == iki) { yaz = IKI_BAYT[i].buyuk; break; }
            }
            if (j + 2 >= n) break;
            out[j++] = (char)(yaz >> 8);
            out[j++] = (char)(yaz & 0xFF);
            p += 2;
            continue;
        }

        /* Tanınmayan dizi: olduğu gibi kopyala — ama ORTASINDAN kesme. */
        if (j + uzunluk >= n) break;
        for (uint32_t i = 0; i < uzunluk && p[i]; i++) out[j++] = (char)p[i];
        p += uzunluk;
    }

    out[j] = '\0';
}
