/**
 * metin.h — UTF-8 Türkçe metin yardımcıları
 *
 * Tasarım tür adlarını BÜYÜK HARF gösteriyor (Oswald, uppercase). Türkçe'de
 * büyütme dile özgü: `i` -> `İ` ve `ı` -> `I`. C'nin `toupper`'ı ikisini de
 * `I` yapar ve "Kızılgerdan" -> "KIZILGERDAN" yerine yanlış harf üretir; bu
 * bir Türk kullanıcının ilk bakışta yakaladığı türden bir hata.
 *
 * Donanımsız (yalnızca stdint/stddef) — host testinde sınanıyor.
 */
#ifndef POKEBIRD_METIN_H
#define POKEBIRD_METIN_H

#include <stdint.h>

/**
 * UTF-8 metni Türkçe kurallarına göre BÜYÜK harfe çevir.
 *
 * Kapsam: ASCII a-z, ve çğıiöşü + şapkalı âîû. Tanınmayan çok baytlı diziler
 * OLDUĞU GİBİ kopyalanıyor (bozmaktansa dokunmamak yeğ).
 * Çıktı her zaman sonlandırılıyor; sığmayan kısım kesiliyor ve UTF-8 dizisi
 * ORTASINDAN kesilmiyor.
 */
void pb_turkce_buyut(const char *utf8, char *out, uint32_t n);

#endif /* POKEBIRD_METIN_H */
