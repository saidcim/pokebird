/**
 * ekran_dinleme.h — EKRAN 0 · DİNLEME
 *
 * Sol 384 piksel (LVGL dilim 0..2): duruma göre başlık + sese en yakın üç tür,
 * her biri ad + bilimsel ad + güven çubuğu. Sağ 256 piksel (dilim 3..4)
 * spektrograma ait ve panele DOĞRUDAN yazılıyor — bu dosya oraya hiç
 * dokunmuyor (bkz. lv_port.c, dilim sahipliği).
 */
#ifndef POKEBIRD_EKRAN_DINLEME_H
#define POKEBIRD_EKRAN_DINLEME_H

#include "lvgl.h"
#include "ui/arayuz.h"

/** Ekranı kur ve döndür (LVGL'e yüklemek çağırana ait). */
lv_obj_t *pb_ekran_dinleme_olustur(void);

/** İçeriği tazele — yalnızca değişen etiketler yeniden yazılıyor. */
void pb_ekran_dinleme_guncelle(const pb_sonuc_gorunum_t *g);

/** Kayıt butonunun görünümünü ayarla (dolu kare = dinliyor, daire = boşta). */
void pb_ekran_dinleme_kayit_ayarla(bool kayitta);

#endif /* POKEBIRD_EKRAN_DINLEME_H */
