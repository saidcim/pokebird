/**
 * ekran_dinleme.h — EKRAN 0 · DİNLEME
 *
 * Sol 384 piksel (LVGL dilim 0..2): duruma göre başlık + sese en yakın üç tür,
 * her biri ad + bilimsel ad + güven çubuğu. Sağ 256 piksel (dilim 3..4)
 * spektrograma ait ve panele DOĞRUDAN yazılıyor — bu dosya oraya hiç
 * dokunmuyor (bkz. lv_port.c, dilim sahipliği).
 */
#ifndef POKEBIRD_SCREEN_LISTEN_H
#define POKEBIRD_SCREEN_LISTEN_H

#include "lvgl.h"
#include "ui/interface.h"

/** Ekranı kur ve döndür (LVGL'e yüklemek çağırana ait). */
lv_obj_t *pb_screen_listen_create(void);

/** İçeriği tazele — yalnızca değişen etiketler yeniden yazılıyor. */
void pb_screen_listen_update(const pb_result_view_t *g);

/** Kayıt butonunun görünümünü ayarla (dolu kare = dinliyor, daire = boşta). */
void pb_screen_listen_set_recording(bool kayitta);

#endif /* POKEBIRD_SCREEN_LISTEN_H */
