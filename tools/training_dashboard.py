#!/usr/bin/env python3
"""
training_dashboard.py — watch a running training log and produce a
self-refreshing HTML dashboard. A SEPARATE PROCESS: it does not touch the
training script at all, it only reads the log file stdout was redirected to.

WHY SEPARATE: train_species.py / train_binary.py have their own internal
write_dashboard() function, but that one only runs if the script is RESTARTED.
If a dashboard is wanted for a training run that is ALREADY going (say a
train_binary.py started in the background), the only way to get one without
disturbing it is to tail the log from the outside.

Usage:
    python tools/training_dashboard.py --log /tmp/train_binary_log.txt \
        --out tools/training_dashboard.html --title "Stage-1 binary net"

The two formats it recognises:
  train_binary.py  : "epoch N/M  loss K  val acc A%  bird-recall G%  negative-specificity O%  Ss"
  train_species.py : "epoch N/M  loss K  val top-1 A%  top-3 B%  Ss"

It stops when a "best" line shows up in the log, or writes once and exits if
--one-shot is given (for CI / automated checks).
"""

import argparse
import re
import time

BINARY_PATTERN = re.compile(
    r"epoch\s+(\d+)/(\d+)\s+loss\s+([\d.]+)\s+val acc ([\d.]+)%\s+"
    r"bird-recall ([\d.]+)%\s+negative-specificity ([\d.]+)%\s+(\d+)s")

SPECIES_PATTERN = re.compile(
    r"epoch\s+(\d+)/(\d+)\s+loss\s+([\d.]+)\s+val top-1 ([\d.]+)%\s+"
    r"top-3 ([\d.]+)%\s+(\d+)s")


def parse_lines(text):
    """Turn the log text into a list of (epoch, total_epochs, loss,
    [metrics...], seconds). Tells the binary and species formats apart on its
    own."""
    binary = [(int(m[0]), int(m[1]), float(m[2]), float(m[3]), float(m[4]),
               float(m[5]), int(m[6])) for m in BINARY_PATTERN.findall(text)]
    if binary:
        return "binary", binary
    species = [(int(m[0]), int(m[1]), float(m[2]), float(m[3]), float(m[4]),
            int(m[5])) for m in SPECIES_PATTERN.findall(text)]
    return "species", species


def line(value, colour, lo_max=None, hi_max=None):
    if not value:
        return ""
    lo = min(value) if lo_max is None else lo_max
    hi = max(value) if hi_max is None else hi_max
    if hi - lo < 1e-9:
        hi = lo + 1
    n = len(value)
    p = " ".join(
        f"{40 + 660 * (i / max(n - 1, 1)):.1f},"
        f"{180 - 160 * ((v - lo) / (hi - lo)):.1f}"
        for i, v in enumerate(value))
    return (f'<polyline fill="none" stroke="{colour}" stroke-width="2.5" '
            f'points="{p}"/>')


def build_html(kind, rows, title, log_path, done):
    if not rows:
        body = "<p class='alt'>No epoch has finished yet, waiting…</p>"
        last = None
        total_epochs = 0
    else:
        last = rows[-1]
        total_epochs = last[1]

    refresh = "" if done else '<meta http-equiv="refresh" content="5">'

    if kind == "binary":
        loss = [s[2] for s in rows]
        acc = [s[3] for s in rows]
        recall = [s[4] for s in rows]
        spec = [s[5] for s in rows]
        boxes = f"""
<div class="k"><span>val acc</span><b>{last[3]:.2f}%</b></div>
<div class="k"><span>bird-recall</span><b>{last[4]:.2f}%</b></div>
<div class="k"><span>negative-specificity</span><b>{last[5]:.2f}%</b></div>
<div class="k"><span>loss</span><b>{last[2]:.4f}</b></div>""" if last else ""
        chart = (f'{line(spec, "#63a8ff", 0, 100)}{line(recall, "#5ed17f", 0, 100)}'
                 f'{line(acc, "#e0803c", 0, 100)}') if rows else ""
        legend = ('<span><i style="background:#5ed17f"></i>bird-recall</span>'
                  '<span><i style="background:#63a8ff"></i>negative-specificity</span>'
                  '<span><i style="background:#e0803c"></i>acc</span>')
        headers = "<th>epoch</th><th>loss</th><th>acc</th><th>recall</th><th>specificity</th>"
        row_html = "".join(
            f"<tr><td>{s[0]}</td><td>{s[2]:.4f}</td><td>{s[3]:.2f}%</td>"
            f"<td>{s[4]:.2f}%</td><td>{s[5]:.2f}%</td></tr>"
            for s in reversed(rows[-25:]))
    else:
        loss = [s[2] for s in rows]
        t1 = [s[3] for s in rows]
        t3 = [s[4] for s in rows]
        boxes = f"""
<div class="k"><span>val top-1</span><b>{last[3]:.2f}%</b></div>
<div class="k"><span>val top-3</span><b>{last[4]:.2f}%</b></div>
<div class="k"><span>loss</span><b>{last[2]:.4f}</b></div>""" if last else ""
        chart = (f'{line(t3, "#63a8ff", 0, 100)}{line(t1, "#5ed17f", 0, 100)}'
                 f'{line(loss, "#e0803c")}') if rows else ""
        legend = ('<span><i style="background:#5ed17f"></i>top-1</span>'
                  '<span><i style="background:#63a8ff"></i>top-3</span>'
                  '<span><i style="background:#e0803c"></i>loss</span>')
        headers = "<th>epoch</th><th>loss</th><th>top-1</th><th>top-3</th>"
        row_html = "".join(
            f"<tr><td>{s[0]}</td><td>{s[2]:.4f}</td><td>{s[3]:.2f}%</td><td>{s[4]:.2f}%</td></tr>"
            for s in reversed(rows[-25:]))

    percent = 100.0 * len(rows) / max(total_epochs, 1)
    status = "done" if done else f"epoch {len(rows)}/{total_epochs}"

    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>{title}</title>
{refresh}
<style>
 body{{font:14px/1.5 system-ui,sans-serif;margin:0;padding:24px;
      background:#11151a;color:#dfe6ee}}
 h1{{font-size:19px;margin:0 0 4px}} .alt{{color:#8b98a6;font-size:13px}}
 .boxes{{display:flex;gap:12px;flex-wrap:wrap;margin:18px 0}}
 .k{{background:#1a2028;border:1px solid #262f3a;border-radius:10px;
     padding:12px 16px;min-width:120px}}
 .k b{{display:block;font-size:22px;font-weight:600;margin-top:2px}}
 .k span{{color:#8b98a6;font-size:12px;text-transform:uppercase;
          letter-spacing:.04em}}
 .bar{{height:8px;background:#232c36;border-radius:5px;overflow:hidden}}
 .bar div{{height:100%;background:linear-gradient(90deg,#3ba55d,#5ed17f)}}
 svg{{background:#1a2028;border:1px solid #262f3a;border-radius:10px}}
 table{{border-collapse:collapse;margin-top:16px;font-variant-numeric:tabular-nums}}
 th,td{{padding:4px 14px 4px 0;text-align:right;border-bottom:1px solid #232c36}}
 th{{color:#8b98a6;font-weight:500;text-align:right}}
 td:first-child,th:first-child{{text-align:left}}
 .leg i{{display:inline-block;width:11px;height:3px;vertical-align:middle;
         margin-right:5px}}
 .leg span{{margin-right:16px;color:#8b98a6;font-size:12px}}
</style></head><body>
<h1>{title}</h1>
<div class="alt">log: {log_path} · {status} {'' if done else '· the page refreshes every 5 s'}</div>
<div class="boxes">{boxes}</div>
<div class="bar"><div style="width:{percent:.1f}%"></div></div>
<p class="leg">{legend}</p>
<svg viewBox="0 0 740 200" width="100%" height="200">
 <line x1="40" y1="180" x2="700" y2="180" stroke="#2e3945"/>
 <line x1="40" y1="20" x2="700" y2="20" stroke="#2e3945" stroke-dasharray="3 4"/>
 {chart}
 <text x="6" y="184" fill="#8b98a6" font-size="11">0</text>
 <text x="6" y="24" fill="#8b98a6" font-size="11">100</text>
</svg>
<table><tr>{headers}</tr>{row_html}</table>
{body if not rows else ''}
</body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True, help="the training log file to watch")
    ap.add_argument("--out", default="tools/training_dashboard.html")
    ap.add_argument("--title", default="PokeBird - training")
    ap.add_argument("--span", type=float, default=3.0, help="refresh seconds")
    ap.add_argument("--one-shot", action="store_true",
                    help="write once and exit (without watching)")
    a = ap.parse_args()

    print(f"watching: {a.log}  ->  {a.out}  (stop with Ctrl+C)")
    while True:
        try:
            with open(a.log, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except FileNotFoundError:
            text = ""
        kind, rows = parse_lines(text)
        done = ("best (" in text) or ("best val top-1" in text)
        html = build_html(kind, rows, a.title, a.log, done)
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(html)
        valid_epochs = rows[-1][0] if rows else 0
        total = rows[-1][1] if rows else 0
        print(f"  wrote: epoch {valid_epochs}/{total}"
              f"{'  [DONE]' if done else ''}")
        if done or a.one_shot:
            break
        time.sleep(a.span)


if __name__ == "__main__":
    main()
