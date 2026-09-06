# M8 — Saha Kalibrasyonu Protokolü

> Belgrad Ormanı · Validebağ Korusu · (ayrı tur) İstanbul gürültüsü
>
> Bu belge tura **çıkmadan önce** okunacak. Amaç, turdan elde anekdot değil
> **ölçüm** ile dönmek. Projedeki her önemli sayı ölçülerek konuldu
> (`models/thresholds.txt`, `models/species_net_report.txt`); saha da öyle olmalı.

---

## 0. Neye "kalibrasyon" diyoruz — ve neye demiyoruz

Sahada **mutlak doğru (ground truth) yok.** Ormanda hangi kuşun öttüğünü
saniyesi saniyesine bilen kimse yok; BirdNET de yanılıyor. O yüzden
ölçtüğümüz şey **doğruluk değil, iki bağımsız tanıyıcının UYUMU**:

| Kova | Anlamı | Ne işe yarar |
|---|---|---|
| **ortak** | Cihaz tür dedi, BirdNET aynı türü aynı anda duydu | Sahada çalıştığının kanıtı |
| **cihaz fazla** | Cihaz tür dedi, BirdNET o türü hiç duymadı | **Yanlış alarm adayı** → negatif madenciliği (§9f-4) |
| **BirdNET fazla** | BirdNET duydu, cihaz hiç demedi | **Kaçırma adayı** → eşik fazla yüksek olabilir |
| **kapsam dışı** | BirdNET 178 listemizde olmayan bir tür duydu | Kaçırma **değil**; liste kararının sonucu |

Bu dördü ayrı ayrı raporlanır. Tek bir yüzdeye indirgemek yanıltıcı olurdu.

> ⚠ **BirdNET referans, hakem değil.** "Cihaz fazla" kovasındaki her satır
> otomatik olarak hata değil — cihaz haklı, BirdNET sağır kalmış olabilir.
> O kovadaki sesler **dinlenerek** ayrıştırılır (bkz. §5).

---

## 1. Cihazın bugünkü kısıtları — turu bunlar şekillendiriyor

| Yok olan | Sonucu |
|---|---|
| **SD kart** (M7 adım 5 yapılmadı) | Cihaz hiçbir şey kaydedemiyor. Tespitler yalnız RAM'de. |
| **RTC** (M7 adım 4 yapılmadı) | Cihazın saat kavramı yok; seri porta bastığı `[ms]` **açılıştan** beri geçen süre. |
| **Ham ses arşivi** | Cihazın *duyduğu* ses saklanamıyor; sonradan yeniden analiz edilemez. |

**Bu yüzden turun kaydedicisi dizüstü bilgisayardır.** Cihaz USB ile
bilgisayara bağlı gider; `tools/saha_kayit.py` seri porta düşen her kararı
duvar saatiyle damgalayıp diske yazar. Firmware'e tek satır eklemek
gerekmiyor — sonuç ekranı bu satırları **zaten** basıyor
([`src/main.c:2813`](../src/main.c#L2813)):

```
  [123456 ms] TUR            grtwoo  Büyük Ağaçkakan  %72.3
```

> Dizüstü olmadan da gidilebilir (cihaz pille tek başına çalışıyor) ama o
> zaman elde yalnızca ekranı gördüğünüz anlar kalır — **ölçüm değil,
> izlenim.** İlk tur ölçüm turu olsun; ikinci tur keyfî gezilebilir.

---

## 2. Tur öncesi kontrol listesi

### Donanım
- [ ] PokeBird cihazı, **HEAD'in derlenmiş `.uf2`'si karta yüklü**
- [ ] USB-C kablo (veri geçiren, sadece şarj eden değil)
- [ ] Dizüstü + tam şarj, veya powerbank
- [ ] Telefon, **en az 4 GB boş yer**, uçak kipi (arama turu bölmesin)
- [ ] Yedek: powerbank + kısa USB kablo

### Yazılım (evden çıkmadan sınanacak)
- [ ] `python tools/saha_kayit.py --port COM13 --yer deneme` → 30 saniye
      çalıştır, elini çırp, Ctrl+C. `saha/…/cihaz.csv` içinde `ISARET` ve
      birkaç satır olmalı. **Bu adım sahada ilk kez denenmeyecek.**
- [ ] `.venv-birdnet` ortamı ayakta mı: `.venv-birdnet\Scripts\python -c "import birdnet_analyzer"`
- [ ] `ffmpeg` var mı (telefon m4a → wav çevrimi için)
- [ ] Doğru COM portu not edildi

### Bilinmesi gerekenler
- Cihazın ekranda tür adı yazma eşiği **0,60 giriş / 0,35 çıkış**
  ([`src/ai/decision.h:41`](../src/ai/decision.h#L41)) — `models/thresholds.txt`'ten
  ölçülerek konuldu (8 pencere): isabet %85,4 · kapsam %35,6 · yanlış
  alarm %2,7. Karar için gereken en az pencere `PB_DECISION_MIN_WINDOWS = 3`,
  tür ekranda `PB_DECISION_HOLD_MS = 5` saniye kalıyor.
  **Saha bu sayıları düşürecek**; ne kadar düşürdüğü bu turun asıl sorusu.
- Cihaz `dinliyor` / `SES ALGILANDI` / `olabilir` / `TUR` kiplerini
  basıyor. `olabilir` kipi de günlüğe düşüyor ama uyum hesabına **yalnız
  `TUR` giriyor** — ekranda tür adı yazdığımız an o.

---

## 3. Turda ne yapılacak

### Yer ve zaman
- **Belgrad Ormanı** — orman ötücüleri (guguk, sarıasma, ağaçkakan,
  tırmaşıkkuş). `lastsession.md`'de not edildiği gibi bunlar *duyuluyor ama
  görülmüyor* — cihazın en çok değer kattığı sınıf tam olarak bu.
- **Saat**: gün doğumundan sonraki ilk 2 saat (dawn chorus). Bu bir tercih
  değil, ötüş yoğunluğu sabah en yüksek; aynı sürede kat kat çok veri.
- **Hava**: rüzgârsız. Rüzgâr mikrofonda geniş bantlı gürültü demek, Aşama-0
  kapısını sürekli açık tutar ve turu boşa çıkarır.

### Kurulum (her durakta aynı)
1. Telefonu ve cihazı **yan yana, aynı yöne bakacak** şekilde koy.
   İkisi farklı yerde durursa uyumsuzluğun kaynağı model mi konum mu
   ayrılamaz.
2. Telefonda **kesintisiz** ses kaydını başlat (tüm tur tek dosya).
3. Dizüstünde:
   ```bash
   python tools/saha_kayit.py --port COM13 --yer belgrad --not "06:40, ruzgarsiz, 12C"
   ```
4. **ENTER'a bas ve aynı anda ELLERİNİ ÇIRP.** Bu, telefon kaydı ile
   bilgisayarın saatini birbirine bağlayan tek köprü. Atlanırsa tur
   zaman ekseninde hizalanamaz ve **ölçüm yapılamaz.**

### Tur boyunca
- Her 20-30 dakikada bir ENTER + çırpma (ara işaretler; biri kaçarsa yedek).
- Cihaz ekranda bir tür yazdığında ve **sen de o kuşu duyduysan/gördüysen**,
  yüksek sesle telefona söyle: *"cihaz kızılgerdan dedi, ben de duydum"*.
  Bu, ses kaydına gömülü insan etiketidir — sonradan altın değerinde.
- Cihaz saçmaladığında da söyle: *"burada sadece rüzgâr var"*.
- **Turun sonunda son bir ENTER + çırpma**, sonra Ctrl+C. Son çırpma
  telefon–PC saat kaymasını ölçmeye yarıyor.

### Süre hedefi
En az **90 dakika kesintisiz kayıt**. Daha kısası istatistik için zayıf:
0,60 eşiğinde cihaz saatte belki 15-40 kez tür yazacak.

---

## 4. Turdan sonra — bilgisayarda

```bash
# 1) Telefon kaydini WAV'a cevir (BirdNET m4a okumaz)
ffmpeg -i telefon.m4a -ar 48000 -ac 1 saha/20260901_0640_belgrad/ses/telefon.wav
```

```bash
# 2) Cirpmanin kayittaki saniyesini bul (Audacity'de en yuksek tepe)
#    ilk cirpma -> --isaret-ses, son cirpma -> --isaret2-ses
```

```bash
# 3) BirdNET'i kayda calistir. birdnet_run.py <girdi>/<altdizin> bekliyor;
#    ses/ altdizini bu yuzden var.
.venv-birdnet/Scripts/python tools/birdnet_run.py --girdi saha/20260901_0640_belgrad --tur ses --out saha/20260901_0640_belgrad/birdnet --isci 1
```

```bash
# 4) Uyumu olc
python tools/saha_karsilastir.py --oturum saha/20260901_0640_belgrad --birdnet saha/20260901_0640_belgrad/birdnet/ses/telefon.BirdNET.results.csv --isaret-ses 12.4 --isaret2-ses 5412.9
```

Çıktı: ekrana rapor + `saha/…/uyum.csv`.

---

## 5. Sonuçla ne yapılacak (turun asıl kazancı)

1. **"cihaz fazla" listesindeki her satırı dinle.** Telefon kaydında o
   saniyeye git.
   - Gerçekten kuş yoksa → o klip **negatif eğitim örneği**. §9f-4'te M8'e
     ertelenen negatif saha turunun ta kendisi bu; ESC-50 yerine cihazın
     kendi kanalından gelen gürültü kat kat değerli
     (`data/negatif/` altına, `esc50_kayitlar.csv` düzeninde).
   - Kuş varsa ama başka tür → **karıştırma çifti**. Bunlar
     `lastsession.md`'de sözü geçen *tür grupları* birleştirmesinin
     gerekçesini oluşturur.
2. **"BirdNET fazla" çoksa** eşik yüksek demektir. `tools/measure_thresholds.py`'yi
   saha verisiyle yeniden çalıştırıp
   [`src/ai/decision.h`](../src/ai/decision.h)'deki üç sabiti güncelle
   (dosyanın başlığı zaten bunu emrediyor).
3. **Sayıları `DEVLOG.md` ve `SUBMISSION.md`'ye işle.** İkisi de şu an
   "test-set accuracy is not field accuracy" diyor — turdan sonra bu cümle
   ölçülmüş bir sayıyla değişebilir.
4. `lastsession.md`'ye §9 girdisi: ne ölçüldü, ne işe yaramadı.

---

## 6. Ayrı ve daha ucuz bir tur: İstanbul gürültüsü (§9f-4)

Bu **cihazsız** yapılabilir, yalnız telefonla, herhangi bir gün:

- ezan, vapur düdüğü, simitçi, martı, trafik, inşaat, cami avlusu, sahil
- her biri 2-5 dakika, tek tek dosyalar
- WAV'a çevrilip `data/negatif/` altına, Aşama-1 ikili ağın negatif
  sınıfına eklenir

Şu an negatif sınıfın tamamı ESC-50 (Amerikan/Avrupa ev-sokak sesleri) ve
lisansı **ticari kullanıma kapalı** (`lastsession.md`, ESC-50 lisansı
satırı). İstanbul'un kendi sesleriyle değiştirmek hem doğruluğu artırır hem
o lisans kısıtını kaldırır. **Yatırım/getiri oranı en yüksek iş bu olabilir
ve bir sabahlık.**

---

## 7. Bu turda YAPILMAYACAKLAR

- **Sahada kod yazılmayacak.** Bir şey bozulursa not al, evde düzelt. Ormanda
  derleyip karta yükleme turu yakar.
- **Eşikler sahada elle değiştirilmeyecek.** Değiştirilirse turun ilk yarısı
  ile ikinci yarısı karşılaştırılamaz hâle gelir.
- **Hoparlörden referans ses çalma bu turda yok.** Ayrı bir iş; canlı ötüşle
  aynı kayda karışırsa iki ölçümü de bozar.
