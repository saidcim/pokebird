#!/usr/bin/env python3
r"""
xc_fetch.py — count and download Xeno-canto recordings.

It does two jobs:

  --survey   Count how many Xeno-canto recordings exist for every species in
             the pool and update species_istanbul.csv with the xc_count_*
             columns and the final `status`. THE FINAL ~110-SPECIES LIST IS
             DECIDED HERE.

  --download Download the recordings of the species on the final list
             (quality A/B first).

WHY THIS IS THE NARROWING CRITERION: the GBIF record count measures "how many
people saw and reported it", not how recognisable a species is by sound —
waterbirds sit in the open and are easy to see, while forest songbirds are
heard and not seen. Getting down to 110 by raising the GBIF threshold cut
exactly the species we were aiming for, such as the Common Cuckoo, the
Eurasian Golden Oriole and the Middle Spotted Woodpecker (measured; see the
note at the end of species_list.py). The Xeno-canto count answers two
questions at once: is the species recognised by sound, and do we have the
data to teach that sound.

AN API KEY IS REQUIRED:
  The Xeno-canto API v2 was shut down; v3 wants a key. It is free:
  https://xeno-canto.org/account

  The key is read from three places, in this order:
      1. the --key argument
      2. the XC_KEY environment variable
      3. the data/.xc_key file        <- PREFERRED

  The file is preferred because the key then never lands in shell history, a
  screenshot or a chat log. It is in .gitignore, so it does not reach the
  repository either. To create it (PowerShell):

      "YOUR-KEY" | Out-File -Encoding ascii -NoNewline data\.xc_key

LICENCE — ND RECORDINGS ARE NOT TAKEN:
  Xeno-canto recordings are Creative Commons but not all alike. Measured over
  the Common Cuckoo's first 300 A/B recordings:
      by-nc-sa 201 / by-nc-nd 34 / by-nc 4 / CC0 2 / by 1 / by-sa 1
  **ND = NoDerivatives**, so processing the work and distributing the result
  is forbidden. Whether training a model on such a recording counts as a
  derivative work is arguable; for 14% of the data that risk is not worth
  taking. ND-licensed recordings are therefore NOT DOWNLOADED (--nd-include
  turns them on, at the user's own responsibility).

  The overwhelming majority of what remains is BY-NC-SA: non-commercial plus
  share-alike. Compatible with the project's personal use but not with
  commercial distribution — the same class of problem as BirdNET's CC
  BY-NC-SA restriction.

  The licence, recordist and XC id of every downloaded file are written to
  `data/xc/records.csv` — keep it, for the attribution obligation.

GEOGRAPHY — EUROPE FIRST:
  There are practically no recordings from Turkey (measured: 1 for the Common
  Cuckoo, 0 for the European Robin, 4 for the Great Tit). But European
  recordings are plentiful and about 90% of the world total is European
  anyway. Since regional dialect in bird song is a real phenomenon,
  recordings from the breeding range are preferred: `area:europe`. For a
  species without enough European recordings it falls back to the whole
  world.
"""

import argparse
import csv
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import csv_compat  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
XC_DIR = os.path.join(DATA, "xc")
WAV_DIR = os.path.join(DATA, "wav")      # the output of xc_convert.py
CACHE = os.path.join(DATA, "cache")
CSV_PATH = os.path.join(DATA, "species_istanbul.csv")

API = "https://xeno-canto.org/api/3/recordings"
UA = {"User-Agent": "PokeBird/0.1 (personal research project)"}

# == The final elimination rule ===========================================
#
# A single threshold DOES NOT WORK, and that was measured from both sides:
#
#   GBIF (sightings) alone: at 912 the Common Cuckoo, the Eurasian Golden
#     Oriole and the Middle Spotted Woodpecker are cut while flamingos and
#     gulls stay — GBIF measures "how many people saw and reported it", not
#     "does it sing".
#
#   XC (audio data) alone: at 60 the Common Hoopoe (1,680 GBIF records and an
#     extremely distinctive "hoo-poo" call), the Eurasian Kestrel, the
#     Mallard and the Lesser Spotted Woodpecker are cut — species that are
#     common in Istanbul but thin on Xeno-canto.
#
# So the two dimensions are combined: species that are COMMON in Istanbul
# stay on the list even with little audio (the user really will hear them),
# while RARE species get in only when there is plenty of audio (without it
# the class cannot be learned anyway).
COMMON_GBIF = 800       # a species with this many Istanbul records is "common"
DEFAULT_THRESHOLD = 30  # minimum XC A/B recordings for a common species
RARE_THRESHOLD = 100    # minimum XC A/B recordings for a species that is not


def is_nd(licence_url):
    """Is the licence ND (NoDerivatives)? The URL looks like
    https://creativecommons.org/licenses/by-nc-nd/4.0/"""
    if not licence_url:
        return False       # licence unknown: 'lic' is empty in the metadata
    u = licence_url.lower()
    if "publicdomain" in u or "zero" in u:
        return False
    code = u.rstrip("/").split("/licenses/")[-1].split("/")[0] if "/licenses/" in u else u
    return "nd" in code.split("-")


def http_json(url, attempt=3):
    for i in range(attempt):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=90) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read(300).decode("utf-8", errors="replace")
            if e.code in (401, 403):
                sys.exit(f"\n[!] the Xeno-canto key was rejected "
                         f"(HTTP {e.code}).\n"
                         f"    {body}\n"
                         f"    Get one from https://xeno-canto.org/account,\n"
                         f"    put it in XC_KEY or pass it with --key.\n")
            if i == attempt - 1:
                raise
            time.sleep(2 * (i + 1))
        except (urllib.error.URLError, TimeoutError):
            if i == attempt - 1:
                raise
            time.sleep(2 * (i + 1))


def xc_query(key, query, page=1, per_page=1):
    p = urllib.parse.urlencode({"query": query, "key": key,
                                "page": page, "per_page": per_page})
    return http_json(f"{API}?{p}")


def read_csv():
    if not os.path.exists(CSV_PATH):
        sys.exit(f"[!] {CSV_PATH} is missing. Run tools/species_list.py first.")
    with open(CSV_PATH, encoding="utf-8") as f:
        r = csv.DictReader(f)
        return list(r), list(r.fieldnames)


def csv_write(rows, columns):
    with open(CSV_PATH, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


# == Survey ==============================================================

def command_survey(key, threshold, rare_threshold=RARE_THRESHOLD,
                   common_gbif=COMMON_GBIF):
    rows, columns = read_csv()
    pool = [s for s in rows if s["status"] == "included"]
    print(f"{len(pool)} species in the pool. Fetching Xeno-canto counts...")
    print(f"(Europe, quality A/B. A common species [GBIF>={COMMON_GBIF}] needs\n"
          f" >={threshold} recordings, a rare one >={RARE_THRESHOLD})\n")

    os.makedirs(CACHE, exist_ok=True)
    for i, s in enumerate(pool, 1):
        sci = s["scientific_name"]
        cache = os.path.join(CACHE, f"xc_{sci.replace(' ', '_')}.json")

        if os.path.exists(cache):
            with open(cache, encoding="utf-8") as f:
                d = json.load(f)
        else:
            # Only the COUNT is needed: ask for a single recording with
            # per_page=1 and read the numRecordings field. Pulling every page
            # would be pointless.
            #
            # Three numbers: world A/B, Europe A/B, Europe BY-NC-SA. The
            # third shows the licence share — a negative licence filter
            # (-lic:) is not supported by the API (it returns 400), so ND is
            # filtered out from the metadata at download time, not here.
            d = {
                "ab": xc_query(
                    key, f'sp:"{sci}" q:">C"').get("numRecordings", "0"),
                "ab_eu": xc_query(
                    key,
                    f'sp:"{sci}" q:">C" area:europe').get("numRecordings", "0"),
                "ab_sa": xc_query(
                    key, f'sp:"{sci}" q:">C" area:europe lic:"BY-NC-SA"'
                ).get("numRecordings", "0"),
            }
            with open(cache, "w", encoding="utf-8") as f:
                json.dump(d, f)
            time.sleep(0.34)      # be polite to the API

        s["xc_count"] = int(d["ab"])          # world, A/B
        s["xc_count_eu"] = int(d["ab_eu"])    # Europe, A/B
        s["xc_count_sa"] = int(d["ab_sa"])    # Europe, A/B, BY-NC-SA only

        if i % 20 == 0 or i == len(pool):
            print(f"  {i}/{len(pool)}")

    # The final elimination - the combined rule (the reasoning is with the
    # constants at the top of the file). The audio criterion is the European
    # A/B count; where there is nothing in Europe (Asian/African species seen
    # in Istanbul) it falls back to the world total.
    for s in rows:
        if s["status"] != "included":
            for k in ("xc_count", "xc_count_eu", "xc_count_sa"):
                s.setdefault(k, "")
            continue
        eu = int(s.get("xc_count_eu") or 0)
        world = int(s.get("xc_count") or 0)
        effective = eu if eu > 0 else world
        common = int(s.get("gbif_records") or 0) >= common_gbif
        needed = threshold if common else rare_threshold

        if effective < needed:
            s["status"] = "excluded"
            s["reason"] = (
                f"{'common' if common else 'rare'} species, {effective} A/B "
                f"recordings on XC (need {needed}; Europe {eu}, "
                f"world {world})")

    for k in ("xc_count", "xc_count_eu", "xc_count_sa"):
        if k not in columns:
            columns.append(k)
    csv_write(rows, columns)

    final = [s for s in rows if s["status"] == "included"]
    print(f"\n{CSV_PATH}")
    print(f"  FINAL LIST: {len(final)} species")
    if final:
        total_record = sum(int(s["xc_count_eu"] or 0) or int(s["xc_count"] or 0)
                           for s in final)
        print(f"  Total usable A/B recordings: {total_record:,}")
        print("\n  The 10 species with the least audio (the data risk is "
              "here):")
        for s in sorted(final, key=lambda x: int(x["xc_count_eu"] or 0)
                        or int(x["xc_count"] or 0))[:10]:
            eu, dw = int(s["xc_count_eu"] or 0), int(s["xc_count"] or 0)
            print(f"    {eu or dw:5} A/B  "
                  f"{s['english_name'] or s['scientific_name']}"
                  f"{'  (world-wide)' if not eu else ''}")
    if len(final) > 130:
        print(f"\n  [i] {len(final)} species is above the ~110 planned; "
              "--threshold can go up.")
    elif len(final) < 90:
        print(f"\n  [i] {len(final)} species is below the ~110 planned; "
              "--threshold can come down.")


# == Download ============================================================

def command_download(key, per_species, only_ab, nd_include):
    rows, _ = read_csv()
    final = [s for s in rows if s["status"] == "included"]
    if not final:
        sys.exit("[!] the final list is empty. Run "
                 "tools/xc_fetch.py --survey first.")

    os.makedirs(XC_DIR, exist_ok=True)
    record_csv = csv_compat.resolve(os.path.join(XC_DIR, "records.csv"))
    fresh_file = not os.path.exists(record_csv)

    print(f"{len(final)} species, at most {per_species} recordings each.\n")
    total_downloaded = 0

    with open(record_csv, "a", newline="", encoding="utf-8") as kf:
        w = csv.writer(kf)
        if fresh_file:
            w.writerow(["file", "scientific_name", "ebird_code", "xc_id",
                        "quality", "licence", "recordist", "country",
                        "length_s"])

        total_nd_skipped = 0

        for s in final:
            sci = s["scientific_name"]
            species_code = s["ebird_code"] or sci.replace(" ", "_")
            target_directory = os.path.join(XC_DIR, species_code)

            # Do not query a species whose quota is already full. It used to
            # open an empty directory for every species, which gave the
            # impression that the species was downloading again and cost a
            # pointless API query on every round.
            wav_directory = os.path.join(WAV_DIR, species_code)
            have = 0
            if os.path.isdir(wav_directory):
                have += len([f for f in os.listdir(wav_directory)
                             if f.endswith(".wav")])
            if os.path.isdir(target_directory):
                have += len([f for f in os.listdir(target_directory)
                             if f.endswith(".mp3")])
            if have >= per_species:
                print(f"  {s['english_name'] or sci}: {have} recordings "
                      "already here, skipped")
                continue

            os.makedirs(target_directory, exist_ok=True)

            quality = ' q:">C"' if only_ab else ""
            # Gradual relaxation: start with the most relevant and cheapest
            # recordings, and drop the constraints one at a time if the
            # per-species target is not met.
            #
            #   0. Europe + 5-60 s   THIS FIRST: BirdNET is going to cut it
            #                        into 3-second slices anyway, so a
            #                        two-minute recording is not needed.
            #                        Measured - long recordings can reach
            #                        18 MB and slow the download down
            #                        eightfold.
            #   1. Europe + 5-120 s  the breeding range, short recordings.
            #                        Long recordings are mostly silence, a
            #                        waste of both disk and segmentation time
            #                        (measured: 32 s average against 24 s).
            #   2. Europe            for species with few recordings the
            #                        length filter narrows the pool too far
            #                        (the Common Hoopoe drops to 37).
            #   3. world-wide        for species with nothing in Europe.
            queries = [
                f'sp:"{sci}"{quality} area:europe len:5-60',
                f'sp:"{sci}"{quality} area:europe len:5-120',
                f'sp:"{sci}"{quality} area:europe',
                f'sp:"{sci}"{quality}',
            ]

            found, seen = [], set()
            for query in queries:
                page = 1
                while len(found) < per_species:
                    d = xc_query(key, query, page=page, per_page=100)
                    records = d.get("recordings", [])
                    if not records:
                        break
                    for k in records:
                        if k.get("id") in seen:
                            continue
                        seen.add(k.get("id"))
                        found.append(k)
                    if page >= int(d.get("numPages", 1)):
                        break
                    page += 1
                    time.sleep(0.34)
                if len(found) >= per_species:
                    break

            downloaded, nd_skipped = 0, 0
            for k in found:
                if downloaded >= per_species:
                    break
                if not nd_include and is_nd(k.get("lic", "")):
                    nd_skipped += 1
                    continue
                xc_id = k.get("id", "")
                url = k.get("file", "")
                if not url:
                    continue
                path = os.path.join(target_directory, f"XC{xc_id}.mp3")
                # Look at the converted WAV as well: xc_convert.py deletes
                # the mp3 and leaves the WAV. Looking only for the mp3 would
                # re-download every converted species from scratch (26 GB
                # went to waste in one round that way).
                wav_path = os.path.join(WAV_DIR,
                                        os.path.basename(target_directory),
                                        f"XC{xc_id}.wav")
                if os.path.exists(path) or os.path.exists(wav_path):
                    downloaded += 1
                    continue
                try:
                    req = urllib.request.Request(url, headers=UA)
                    with urllib.request.urlopen(req, timeout=120) as r, \
                         open(path, "wb") as out:
                        out.write(r.read())
                except Exception as e:
                    print(f"    [!] XC{xc_id} could not be downloaded: "
                          f"{type(e).__name__}")
                    continue
                w.writerow([os.path.relpath(path, ROOT), sci, s["ebird_code"], xc_id,
                            k.get("q", ""), k.get("lic", ""), k.get("rec", ""),
                            k.get("cnt", ""), k.get("length", "")])
                downloaded += 1
                total_downloaded += 1
                time.sleep(0.1)

            total_nd_skipped += nd_skipped
            print(f"  {s['english_name'] or sci}: {downloaded} recordings"
                  f"{f' (ND skipped: {nd_skipped})' if nd_skipped else ''}")
            kf.flush()

    print(f"\n{total_downloaded} new recordings in total -> {XC_DIR}")
    if total_nd_skipped:
        print(f"{total_nd_skipped} ND-licensed (no-derivatives) recordings "
              "were skipped.")
    print(f"Licence and attribution information: {record_csv}")


KEY_FILE = os.path.join(DATA, ".xc_key")


def find_key(argument):
    """Look for the key in three places, in order. The file is preferred: the
    key then never lands in shell history or on screen."""
    if argument:
        return argument.strip()
    if os.environ.get("XC_KEY"):
        return os.environ["XC_KEY"].strip()
    if os.path.exists(KEY_FILE):
        with open(KEY_FILE, encoding="utf-8-sig") as f:
            return f.read().strip()
    return ""


def main():
    ap = argparse.ArgumentParser(description="PokeBird: Xeno-canto")
    ap.add_argument("--key", default="",
                    help="the Xeno-canto API key (otherwise XC_KEY or "
                         "data/.xc_key)")
    ap.add_argument("--survey", action="store_true",
                    help="fetch recording counts and build the final species list")
    ap.add_argument("--download", action="store_true",
                    help="download the recordings of the final list")
    ap.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD,
                    help="minimum A/B recordings for a common species "
                         f"(default {DEFAULT_THRESHOLD})")
    ap.add_argument("--rare-threshold", type=int, default=RARE_THRESHOLD,
                    help="minimum A/B recordings for a rare species "
                         f"(default {RARE_THRESHOLD})")
    ap.add_argument("--common-gbif", type=int, default=COMMON_GBIF,
                    help="a species with this many GBIF records counts as "
                         f"'common' (default {COMMON_GBIF})")
    ap.add_argument("--per-species", type=int, default=60,
                    help="recordings to download per species (default 60)")
    ap.add_argument("--all-quality", action="store_true",
                    help="download C/D/E recordings as well as A/B")
    ap.add_argument("--nd-include", action="store_true",
                    help="download ND-licensed (no-derivatives) recordings "
                         "too - at your own responsibility")
    args = ap.parse_args()

    key = find_key(args.key)
    if not key:
        sys.exit(
            "\n[!] no Xeno-canto API key was found.\n\n"
            "    API v2 was shut down and v3 wants a key. To get one free:\n"
            "      1. open an account at https://xeno-canto.org/account\n"
            "      2. copy your API key from that same page\n"
            "      3. write the key into the file (PowerShell):\n"
            "           \"KEY\" | Out-File -Encoding ascii -NoNewline "
            "data\\.xc_key\n\n"
            "    Alternatively:  $env:XC_KEY = \"KEY\"   or   --key KEY\n")

    if args.survey:
        command_survey(key, args.threshold, args.rare_threshold,
                       args.common_gbif)
    elif args.download:
        command_download(key, args.per_species, not args.all_quality,
                         args.nd_include)
    else:
        ap.print_help()


if __name__ == "__main__":
    sys.exit(main())
