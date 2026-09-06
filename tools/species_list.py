#!/usr/bin/env python3
"""
species_list.py — step 1: settle the Istanbul species list.

Output: data/species_istanbul.csv (scientific name, Turkish name, English
name, eBird code, GBIF record count, monthly distribution, included/excluded
plus a reason).

DATA SOURCES — neither NEEDS AN API KEY:

  GBIF occurrence facet   How many records exist in Istanbul (GADM TUR.40_1)
                          for each bird species. eBird's own dataset (EOD) is
                          inside GBIF, so this is the practical equivalent of
                          an eBird regional list. eBird's own API wants a key
                          for regional lists (403); GBIF does not.

  eBird taxonomy          Scientific name -> Turkish name + English name +
                          eBird code. With locale=tr the Turkish names come
                          back without a key.

WHY A GBIF RECORD-COUNT THRESHOLD: the plan says "species with fewer than 30
Xeno-canto recordings are dropped". The Xeno-canto API now requires a key (v2
was retired), so that filter lives in `xc_fetch.py`. The GBIF threshold here
answers a different question: "is this species actually seen in Istanbul, or
is it a one-off record?" The two filters do not substitute for each other;
both are needed.

Usage:
    python tools/species_list.py                # species list (fast)
    python tools/species_list.py --monthly      # + monthly distribution (slow, ~390 queries)
    python tools/species_list.py --threshold 50 # change the record-count threshold
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
CACHE = os.path.join(DATA, "cache")

GBIF = "https://api.gbif.org/v1"
EBIRD = "https://api.ebird.org/v2"
AVES_TAXON_KEY = 212          # GBIF: the class Aves
ISTANBUL_GADM = "TUR.40_1"    # GADM province code — TUR.35_1 is Gumushane,
                              # do not confuse the two

UA = {"User-Agent": "PokeBird/0.1 (personal research project)"}


# ── Excluded species ──────────────────────────────────────────────────────
#
# This list is maintained BY HAND because there is no automatic criterion for
# it. Each line carries its reason; when adding a species, write the reason
# too, or in six months nobody will know why it was dropped.

# Domestic, escaped-from-a-cage or non-established exotics: they have records
# in Istanbul but they are not "wild Istanbul birds". Having the model learn
# them would waste class budget.
EXOTIC = {
    "Psittacula krameri":      "escaped cage parakeet (established but exotic)",
    "Myiopsitta monachus":     "escaped cage parakeet",
    "Melopsittacus undulatus": "budgerigar, a cage bird",
    "Nymphicus hollandicus":   "cockatiel, a cage bird",
    "Serinus canaria":         "canary, a cage bird",
    "Gallus gallus":           "domestic chicken",
    "Anser anser domesticus":  "domestic goose",
    "Cairina moschata":        "domestic muscovy duck",
    "Pavo cristatus":          "peafowl, domestic",
    "Numida meleagris":        "guineafowl, domestic",
    "Phasianus colchicus":     "released game bird",
    "Estrilda astrild":        "escaped cage finch",
    "Euplectes afer":          "escaped cage weaver",
    "Columba livia domestica": "domestic pigeon",
}

# Species for which recognition by sound is meaningless or impossible. Note:
# there is no such thing as a "silent bird" — the question is whether it can
# be TOLD APART WITH A MICROPHONE.
NOT_IDENTIFIABLE_BY_SOUND = {
    # Species that stay far out at sea, hundreds of metres from shore, and
    # generally fly silently.
    "Calonectris diomedea": "far out at sea, inaudible from land",
    "Puffinus yelkouan":    "far out at sea, inaudible from land",
    "Hydrobates pelagicus": "far out at sea, inaudible from land",
}


def cache_get(name, produce):
    """Cache an API result on disk, so the script can be re-run and the API is
    not hammered needlessly; re-fetching 390 species queries takes minutes."""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    data = produce()
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    return data


def http_json(url, attempt=3):
    for i in range(attempt):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=90) as r:
                return json.loads(r.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
            if i == attempt - 1:
                raise
            time.sleep(2 * (i + 1))


# ── GBIF ───────────────────────────────────────────────────────────────────

def gbif_istanbul_species():
    """Bird species in Istanbul and their record counts: {speciesKey: count}."""
    url = (f"{GBIF}/occurrence/search?gadmGid={ISTANBUL_GADM}"
           f"&taxonKey={AVES_TAXON_KEY}&limit=0&hasCoordinate=true"
           f"&facet=speciesKey&facetLimit=1200")
    d = http_json(url)
    counts = d.get("facets", [{}])[0].get("counts", [])
    return d["count"], {c["name"]: c["count"] for c in counts}


def gbif_species_name(species_key):
    d = http_json(f"{GBIF}/species/{species_key}")
    return d.get("canonicalName") or d.get("scientificName", "")


def gbif_monthly(species_key):
    """The species' monthly record distribution in Istanbul — the raw data for
    a seasonal-prior table. A 12-element list, January = index 0."""
    url = (f"{GBIF}/occurrence/search?gadmGid={ISTANBUL_GADM}"
           f"&taxonKey={AVES_TAXON_KEY}&speciesKey={species_key}"
           f"&limit=0&hasCoordinate=true&facet=month&facetLimit=12")
    d = http_json(url)
    months = [0] * 12
    for c in d.get("facets", [{}])[0].get("counts", []):
        try:
            month = int(c["name"])
            if 1 <= month <= 12:
                months[month - 1] = c["count"]
        except (ValueError, KeyError):
            pass
    return months


# ── eBird ──────────────────────────────────────────────────────────────────

def ebird_taxonomy():
    """Scientific name -> (eBird code, Turkish name, English name).

    The taxonomy is fetched TWICE, once per language: the API returns one name
    per locale and there is no endpoint that gives both at once."""
    def fetch(locale):
        url = f"{EBIRD}/ref/taxonomy/ebird?fmt=json&cat=species"
        if locale:
            url += f"&locale={locale}"
        return http_json(url)

    tr = cache_get("ebird_tax_tr.json", lambda: fetch("tr"))
    en = cache_get("ebird_tax_en.json", lambda: fetch(None))

    en_name = {t["speciesCode"]: t.get("comName", "") for t in en}
    table = {}
    for t in tr:
        sci = t.get("sciName", "")
        code = t.get("speciesCode", "")
        if sci:
            table[sci] = (code, t.get("comName", ""), en_name.get(code, ""))
    return table


# ── Main flow ──────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="PokeBird: Istanbul species list")
    ap.add_argument("--threshold", type=int, default=30,
                    help="minimum GBIF record count (default 30)")
    ap.add_argument("--monthly", action="store_true",
                    help="also fetch the monthly distribution "
                         "(one query per species, slow)")
    ap.add_argument("--out", default=os.path.join(DATA, "species_istanbul.csv"))
    args = ap.parse_args()

    os.makedirs(DATA, exist_ok=True)

    print(f"GBIF: fetching bird records for Istanbul ({ISTANBUL_GADM})...")
    total, counts = cache_get("gbif_istanbul_facet.json", gbif_istanbul_species)
    print(f"  {total:,} records, {len(counts)} distinct species")

    print("Fetching the eBird taxonomy (TR + EN)...")
    taxonomy = ebird_taxonomy()
    print(f"  {len(taxonomy)} species")

    print("Resolving species names...")
    names = cache_get("gbif_species_names.json",
                      lambda: {k: gbif_species_name(k) for k in counts})

    rows = []
    for key, count in sorted(counts.items(), key=lambda kv: -kv[1]):
        sci = names.get(key, "")
        if not sci:
            continue

        code, tr_name, en_common = taxonomy.get(sci, ("", "", ""))

        status, reason = "included", ""
        if sci in EXOTIC:
            status, reason = "excluded", EXOTIC[sci]
        elif sci in NOT_IDENTIFIABLE_BY_SOUND:
            status, reason = "excluded", NOT_IDENTIFIABLE_BY_SOUND[sci]
        elif count < args.threshold:
            status, reason = "excluded", (f"only {count} records in Istanbul "
                                          f"(threshold {args.threshold})")
        elif not code:
            # Absent from the eBird taxonomy means either a subspecies or an
            # outdated name. Including it silently would be wrong: the
            # Xeno-canto query would not match either.
            status, reason = "excluded", "no match in the eBird taxonomy"

        rows.append({
            "ebird_code": code, "scientific_name": sci, "turkish_name": tr_name,
            "english_name": en_common, "gbif_records": count,
            "gbif_species_key": key, "status": status, "reason": reason,
        })

    if args.monthly:
        include = [s for s in rows if s["status"] == "included"]
        print(f"Fetching the monthly distribution ({len(include)} species)...")
        for i, s in enumerate(include, 1):
            key = s["gbif_species_key"]
            months = cache_get(f"monthly_{key}.json",
                               lambda k=key: gbif_monthly(k))
            for month in range(12):
                s[f"month_{month+1:02d}"] = months[month]
            if i % 25 == 0 or i == len(include):
                print(f"  {i}/{len(include)}")

    columns = ["ebird_code", "scientific_name", "turkish_name", "english_name",
               "gbif_records", "gbif_species_key", "status", "reason"]
    if args.monthly:
        columns += [f"month_{m:02d}" for m in range(1, 13)]

    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        w.writeheader()
        for s in rows:
            w.writerow(s)

    include = sum(1 for s in rows if s["status"] == "included")
    excluded = len(rows) - include
    print(f"\n{args.out}")
    print(f"  included {include} species")
    print(f"  excluded {excluded} species")

    if include:
        print("\nThe 10 species with the most records:")
        for s in [x for x in rows if x["status"] == "included"][:10]:
            print(f"  {s['gbif_records']:7,}  "
                  f"{s['english_name'] or s['scientific_name']}")

    # The plan expects roughly 110 species. This list is a POOL; xc_fetch.py
    # does the final narrowing.
    #
    # DO NOT GET DOWN TO 110 BY RAISING THE THRESHOLD — it was measured, and it
    # drops the wrong species: at a threshold of 912 it excludes exactly the
    # woodland singers the device is meant to recognise (Common Cuckoo, 839
    # records; Eurasian Golden Oriole, 817; Middle Spotted Woodpecker, 835;
    # Short-toed Treecreeper, 837) and keeps Greater Flamingo (912), Mute Swan
    # (838) and the gulls. That is because the GBIF record count measures "how
    # many people saw and reported it", not "does it sing and can its song be
    # told apart": waterbirds are out in the open and easy to see, while
    # woodland singers are heard but not seen.
    #
    # The right narrowing criterion is the Xeno-canto recording count, which
    # measures identifiability by sound and the availability of training data
    # at the same time.
    if include > 300:
        print(f"\n[!] {include} species is more than expected — the GBIF query "
              f"may have widened.")
    elif include < 150:
        print(f"\n[!] {include} species is low for a pool. Consider lowering "
              f"--threshold.")
    else:
        print(f"\nThis is a POOL list. The final narrowing to about 110 species\n"
              f"happens in xc_fetch.py, by Xeno-canto recording count (see the\n"
              f"header of this file).")


if __name__ == "__main__":
    sys.exit(main())
