#!/usr/bin/env python3
"""
egitim_pano.py — calisan bir egitim log dosyasini izleyip kendini yenileyen
bir HTML pano uretir. AYRI BIR SUREC: egitim script'ine hic dokunmuyor,
yalnizca stdout'un yonlendirildigi log dosyasini okuyor.

NEDEN AYRI: egit.py/ikili_egit.py'nin kendi ici pano_yaz() fonksiyonu var
ama o SADECE o script YENIDEN BASLATILIRSA calisir. Zaten calismakta olan
bir egitimi (ör. arka planda baslatilmis ikili_egit.py) bozmadan pano
istenirse tek yol disaridan log'u tail'lemek.

Kullanim:
    python tools/training_dashboard.py --log /tmp/ikili_egit_log.txt \
        --out tools/egitim_pano.html --baslik "Asama-1 ikili ag"

Taniyabildigi iki format:
  ikili_egit.py : "devir N/M  kayip K  dogrulama acc %A  kus-geri-cagirma %G  negatif-ozgulluk %O  Ssn"
  egit.py       : "devir N/M  kayip K  dogrulama top-1 %A  top-3 %B  Ssn"

durur: log'da "en iyi" satiri gorununce ya da --bir-kere verilirse tek
seferlik yazip cikar (CI/otomatik kontrol icin).
"""

import argparse
import re
import time

IKILI_DESEN = re.compile(
    r"devir\s+(\d+)/(\d+)\s+kayip\s+([\d.]+)\s+dogrulama acc %([\d.]+)\s+"
    r"kus-geri-cagirma %([\d.]+)\s+negatif-ozgulluk %([\d.]+)\s+(\d+)sn")

TUR_DESEN = re.compile(
    r"devir\s+(\d+)/(\d+)\s+kayip\s+([\d.]+)\s+dogrulama top-1 %([\d.]+)\s+"
    r"top-3 %([\d.]+)\s+(\d+)sn")


def satirlari_ayikla(metin):
    """Log metnini (devir, toplam_devir, kayip, [metrikler...], sure) listesine cevirir.
    Ikili ve tur formatini otomatik ayirt eder."""
    ikili = [(int(m[0]), int(m[1]), float(m[2]), float(m[3]), float(m[4]),
              float(m[5]), int(m[6])) for m in IKILI_DESEN.findall(metin)]
    if ikili:
        return "ikili", ikili
    tur = [(int(m[0]), int(m[1]), float(m[2]), float(m[3]), float(m[4]),
            int(m[5])) for m in TUR_DESEN.findall(metin)]
    return "tur", tur


def cizgi(deger, renk, en_az=None, en_cok=None):
    if not deger:
        return ""
    lo = min(deger) if en_az is None else en_az
    hi = max(deger) if en_cok is None else en_cok
    if hi - lo < 1e-9:
        hi = lo + 1
    n = len(deger)
    p = " ".join(
        f"{40 + 660 * (i / max(n - 1, 1)):.1f},"
        f"{180 - 160 * ((v - lo) / (hi - lo)):.1f}"
        for i, v in enumerate(deger))
    return (f'<polyline fill="none" stroke="{renk}" stroke-width="2.5" '
            f'points="{p}"/>')


def html_uret(tip, satirlar, baslik, log_yolu, bitti):
    if not satirlar:
        govde = "<p class='alt'>Henuz devir tamamlanmadi, bekleniyor…</p>"
        son = None
        toplam_devir = 0
    else:
        son = satirlar[-1]
        toplam_devir = son[1]

    yenile = "" if bitti else '<meta http-equiv="refresh" content="5">'

    if tip == "ikili":
        d = [s[0] for s in satirlar]
        kayip = [s[2] for s in satirlar]
        acc = [s[3] for s in satirlar]
        geri = [s[4] for s in satirlar]
        ozg = [s[5] for s in satirlar]
        kutular = f"""
<div class="k"><span>dogrulama acc</span><b>%{son[3]:.2f}</b></div>
<div class="k"><span>kus-geri-cagirma</span><b>%{son[4]:.2f}</b></div>
<div class="k"><span>negatif-ozgulluk</span><b>%{son[5]:.2f}</b></div>
<div class="k"><span>kayip</span><b>{son[2]:.4f}</b></div>""" if son else ""
        grafik = (f'{cizgi(ozg, "#63a8ff", 0, 100)}{cizgi(geri, "#5ed17f", 0, 100)}'
                  f'{cizgi(acc, "#e0803c", 0, 100)}') if satirlar else ""
        lejant = ('<span><i style="background:#5ed17f"></i>kus-geri-cagirma</span>'
                  '<span><i style="background:#63a8ff"></i>negatif-ozgulluk</span>'
                  '<span><i style="background:#e0803c"></i>acc</span>')
        basliklar = "<th>devir</th><th>kayip</th><th>acc</th><th>geri-cagirma</th><th>ozgulluk</th>"
        satir_html = "".join(
            f"<tr><td>{s[0]}</td><td>{s[2]:.4f}</td><td>%{s[3]:.2f}</td>"
            f"<td>%{s[4]:.2f}</td><td>%{s[5]:.2f}</td></tr>"
            for s in reversed(satirlar[-25:]))
    else:
        d = [s[0] for s in satirlar]
        kayip = [s[2] for s in satirlar]
        t1 = [s[3] for s in satirlar]
        t3 = [s[4] for s in satirlar]
        kutular = f"""
<div class="k"><span>dogrulama top-1</span><b>%{son[3]:.2f}</b></div>
<div class="k"><span>dogrulama top-3</span><b>%{son[4]:.2f}</b></div>
<div class="k"><span>kayip</span><b>{son[2]:.4f}</b></div>""" if son else ""
        grafik = (f'{cizgi(t3, "#63a8ff", 0, 100)}{cizgi(t1, "#5ed17f", 0, 100)}'
                  f'{cizgi(kayip, "#e0803c")}') if satirlar else ""
        lejant = ('<span><i style="background:#5ed17f"></i>top-1</span>'
                  '<span><i style="background:#63a8ff"></i>top-3</span>'
                  '<span><i style="background:#e0803c"></i>kayip</span>')
        basliklar = "<th>devir</th><th>kayip</th><th>top-1</th><th>top-3</th>"
        satir_html = "".join(
            f"<tr><td>{s[0]}</td><td>{s[2]:.4f}</td><td>%{s[3]:.2f}</td><td>%{s[4]:.2f}</td></tr>"
            for s in reversed(satirlar[-25:]))

    yuzde = 100.0 * len(satirlar) / max(toplam_devir, 1)
    durum = "bitti" if bitti else f"devir {len(satirlar)}/{toplam_devir}"

    return f"""<!doctype html><html lang="tr"><head><meta charset="utf-8">
<title>{baslik}</title>
{yenile}
<style>
 body{{font:14px/1.5 system-ui,sans-serif;margin:0;padding:24px;
      background:#11151a;color:#dfe6ee}}
 h1{{font-size:19px;margin:0 0 4px}} .alt{{color:#8b98a6;font-size:13px}}
 .kutular{{display:flex;gap:12px;flex-wrap:wrap;margin:18px 0}}
 .k{{background:#1a2028;border:1px solid #262f3a;border-radius:10px;
     padding:12px 16px;min-width:120px}}
 .k b{{display:block;font-size:22px;font-weight:600;margin-top:2px}}
 .k span{{color:#8b98a6;font-size:12px;text-transform:uppercase;
          letter-spacing:.04em}}
 .cubuk{{height:8px;background:#232c36;border-radius:5px;overflow:hidden}}
 .cubuk div{{height:100%;background:linear-gradient(90deg,#3ba55d,#5ed17f)}}
 svg{{background:#1a2028;border:1px solid #262f3a;border-radius:10px}}
 table{{border-collapse:collapse;margin-top:16px;font-variant-numeric:tabular-nums}}
 th,td{{padding:4px 14px 4px 0;text-align:right;border-bottom:1px solid #232c36}}
 th{{color:#8b98a6;font-weight:500;text-align:right}}
 td:first-child,th:first-child{{text-align:left}}
 .lej i{{display:inline-block;width:11px;height:3px;vertical-align:middle;
         margin-right:5px}}
 .lej span{{margin-right:16px;color:#8b98a6;font-size:12px}}
</style></head><body>
<h1>{baslik}</h1>
<div class="alt">log: {log_yolu} · {durum} {'' if bitti else '· sayfa 5 sn de bir yenileniyor'}</div>
<div class="kutular">{kutular}</div>
<div class="cubuk"><div style="width:{yuzde:.1f}%"></div></div>
<p class="lej">{lejant}</p>
<svg viewBox="0 0 740 200" width="100%" height="200">
 <line x1="40" y1="180" x2="700" y2="180" stroke="#2e3945"/>
 <line x1="40" y1="20" x2="700" y2="20" stroke="#2e3945" stroke-dasharray="3 4"/>
 {grafik}
 <text x="6" y="184" fill="#8b98a6" font-size="11">0</text>
 <text x="6" y="24" fill="#8b98a6" font-size="11">100</text>
</svg>
<table><tr>{basliklar}</tr>{satir_html}</table>
{govde if not satirlar else ''}
</body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True, help="izlenecek egitim log dosyasi")
    ap.add_argument("--out", default="tools/egitim_pano.html")
    ap.add_argument("--baslik", default="PokeBird — egitim")
    ap.add_argument("--aralik", type=float, default=3.0, help="yenileme saniyesi")
    ap.add_argument("--bir-kere", action="store_true",
                    help="tek seferlik yaz ve cik (izlemeden)")
    a = ap.parse_args()

    print(f"izleniyor: {a.log}  ->  {a.out}  (Ctrl+C ile durdurun)")
    while True:
        try:
            with open(a.log, encoding="utf-8", errors="replace") as f:
                metin = f.read()
        except FileNotFoundError:
            metin = ""
        tip, satirlar = satirlari_ayikla(metin)
        bitti = ("en iyi (" in metin) or ("en iyi dogrulama" in metin)
        html = html_uret(tip, satirlar, a.baslik, a.log, bitti)
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(html)
        gecerli_devir = satirlar[-1][0] if satirlar else 0
        toplam = satirlar[-1][1] if satirlar else 0
        print(f"  yazildi: devir {gecerli_devir}/{toplam}"
              f"{'  [BITTI]' if bitti else ''}")
        if bitti or a.bir_kere:
            break
        time.sleep(a.aralik)


if __name__ == "__main__":
    main()
