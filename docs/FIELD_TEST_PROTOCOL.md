# Field Calibration Protocol

> Belgrad Forest · Validebağ Grove · (a separate trip) Istanbul noise
>
> Read this **before** going out. The point is to come back from the trip with
> a **measurement**, not an anecdote. Every important number in this project
> was measured before it was written down (`models/thresholds.txt`,
> `models/species_net_report.txt`); the field should be no different.

---

## 0. What we call "calibration" — and what we do not

There is **no ground truth** in the field. Nobody knows second by second which
bird was singing in the forest, and BirdNET is wrong sometimes too. So what we
measure is **not accuracy but the AGREEMENT of two independent recognisers**:

| Bucket | Meaning | What it is for |
|---|---|---|
| **shared** | the device named a species and BirdNET heard the same one at the same time | evidence that it works in the field |
| **device extra** | the device named a species and BirdNET never heard it | a **false-alarm candidate** -> negative mining |
| **BirdNET extra** | BirdNET heard it and the device never said it | a **miss candidate** -> the threshold may be too high |
| **out of scope** | BirdNET heard a species that is not in our 178 | **not** a miss; a consequence of the list decision |

All four are reported separately. Reducing them to a single percentage would
mislead.

> ⚠ **BirdNET is a reference, not a referee.** Not every row in the "device
> extra" bucket is automatically an error — the device may be right and
> BirdNET deaf. The sounds in that bucket are separated **by listening** (see
> §5).

---

## 1. The device's constraints today — they shape the trip

| What is missing | The consequence |
|---|---|
| **The SD card** | the device can record nothing. Detections live only in RAM. |
| **The RTC** | the device has no notion of the time of day; the `[ms]` it prints to the serial port is the time since **startup**. |
| **A raw audio archive** | what the device *hears* cannot be kept, so it cannot be re-analysed later. |

**That is why the trip's recorder is the laptop.** The device goes out
connected to the computer over USB; `tools/field_log.py` stamps every decision
that reaches the serial port with the wall clock and writes it to disk. Not a
line has to be added to the firmware — the result screen **already** prints
those lines ([`firmware/src/main.c`](../firmware/src/main.c)):

```
  [123456 ms] SPECIES        grtwoo  Great Spotted Woodpecker  72.3%
```

> You can go without the laptop (the device runs on its own on battery) but
> then all you have is the moments you happened to be looking at the screen —
> **an impression, not a measurement.** Let the first trip be the measuring
> trip; the second can be for pleasure.

---

## 2. The pre-trip checklist

### Hardware
- [ ] the PokeBird device, with **HEAD's compiled `.uf2` loaded onto it**
- [ ] a USB-C cable (one that carries data, not charge-only)
- [ ] a laptop, fully charged, or a power bank
- [ ] a phone with **at least 4 GB free**, in flight mode (a call must not cut
      the recording)
- [ ] spares: a power bank plus a short USB cable

### Software (tested before leaving the house)
- [ ] `python tools/field_log.py --port COM13 --place test` -> run it for 30
      seconds, clap, Ctrl+C. `field/…/device.csv` should hold a `MARKER` and a
      few rows. **This step is not to be tried for the first time in the
      field.**
- [ ] the `.venv-birdnet` environment is up:
      `.venv-birdnet\Scripts\python -c "import birdnet_analyzer"`
- [ ] `ffmpeg` is available (for the phone's m4a -> wav conversion)
- [ ] the right COM port is noted down

### What to know
- The threshold at which the device prints a species name is **0.60 to enter /
  0.35 to leave** ([`firmware/src/ai/decision.h`](../firmware/src/ai/decision.h))
  — measured into place from `models/thresholds.txt` (8 windows): precision
  85.4%, coverage 35.6%, false alarms 2.7%. The minimum number of windows for
  a decision is `PB_DECISION_MIN_WINDOWS = 3`, and a species stays on screen
  for `PB_DECISION_HOLD_MS = 5` seconds. **The field will lower these
  numbers**; by how much is the real question of this trip.
- The device prints the modes `listening` / `SOUND DETECTED` / `maybe` /
  `SPECIES`. The `maybe` mode is logged too, but **only `SPECIES` counts**
  towards the agreement — that is the moment a species name goes on screen.

---

## 3. What to do on the trip

### Place and time
- **Belgrad Forest** — forest songbirds (cuckoo, oriole, woodpeckers,
  treecreepers). These are *heard but not seen*, which is exactly the class
  where the device adds the most.
- **Time**: the first 2 hours after sunrise (the dawn chorus). This is not a
  preference; song density is highest in the morning, so the same amount of
  time yields several times the data.
- **Weather**: no wind. Wind means broadband noise in the microphone, which
  holds the stage-0 gate open continuously and wastes the trip.

### Setup (the same at every stop)
1. Put the phone and the device **side by side, facing the same way**. If they
   sit in different places, a disagreement cannot be attributed to the model
   rather than the position.
2. Start an **uninterrupted** recording on the phone (one file for the whole
   trip).
3. On the laptop:
   ```bash
   python tools/field_log.py --port COM13 --place belgrad --note "06:40, no wind, 12C"
   ```
4. **Press ENTER and CLAP YOUR HANDS at the same moment.** This is the only
   bridge tying the phone recording to the computer's clock. Skip it and the
   trip cannot be lined up in time, so **no measurement is possible.**

### Through the trip
- ENTER + a clap every 20–30 minutes (intermediate markers; a spare if one is
  missed).
- When the device puts a species on screen and **you heard or saw that bird
  too**, say so out loud to the phone: *"the device said robin, and I heard it
  too"*. That is a human label embedded in the audio, and it is worth its
  weight in gold afterwards.
- Say so when the device is talking nonsense as well: *"there is only wind
  here"*.
- **One last ENTER + clap at the end of the trip**, then Ctrl+C. The final
  clap is what measures the phone-to-PC clock drift.

### Duration target
At least **90 minutes of uninterrupted recording**. Anything shorter is thin
for statistics: at the 0.60 threshold the device will name a species perhaps
15–40 times an hour.

---

## 4. After the trip — at the computer

```bash
# 1) Convert the phone recording to WAV (BirdNET does not read m4a)
ffmpeg -i phone.m4a -ar 48000 -ac 1 field/20260901_0640_belgrad/audio/phone.wav
```

```bash
# 2) Find the second the clap falls on in the recording (the tallest peak in Audacity)
#    the first clap -> --marker-audio, the last clap -> --marker2-audio
```

```bash
# 3) Run BirdNET over the recording. birdnet_run.py expects <inp>/<subdir>,
#    which is why the audio/ subdirectory exists.
.venv-birdnet/Scripts/python tools/birdnet_run.py --inp field/20260901_0640_belgrad --species audio --out field/20260901_0640_belgrad/birdnet --workers 1
```

```bash
# 4) Measure the agreement
python tools/field_compare.py --session field/20260901_0640_belgrad --birdnet field/20260901_0640_belgrad/birdnet/audio/phone.BirdNET.results.csv --marker-audio 12.4 --marker2-audio 5412.9
```

Output: a report on screen plus `field/…/agreement.csv`.

---

## 5. What to do with the result (the real gain of the trip)

1. **Listen to every row in the "device extra" list.** Go to that second in
   the phone recording.
   - If there really is no bird -> that clip is a **negative training
     example**. This is exactly the negative field trip that was deferred;
     noise coming through the device's own channel is worth far more than
     ESC-50 (put it under `data/negative/`, in the layout of
     `esc50_records.csv`).
   - If there is a bird but a different species -> a **confusion pair**. These
     are what justify merging species into groups.
2. **If "BirdNET extra" is large**, the threshold is too high. Re-run
   `tools/measure_thresholds.py` on the field data and update the three
   constants in
   [`firmware/src/ai/decision.h`](../firmware/src/ai/decision.h) (the header
   of that file already says so).
3. **Write the numbers into the README.** It currently says that test-set
   accuracy is not field accuracy — after the trip that sentence can carry a
   measured number.

---

## 6. A separate and cheaper trip: Istanbul noise

This can be done **without the device**, with just a phone, on any day:

- the call to prayer, ferry horns, a simit seller, gulls, traffic,
  construction, a mosque courtyard, the seafront
- 2–5 minutes each, as separate files
- converted to WAV and added under `data/negative/`, into the stage-1 binary
  net's negative class

At the moment the entire negative class is ESC-50 (American and European
household and street sounds) and its licence is **closed to commercial use**.
Replacing it with Istanbul's own sounds would raise accuracy and lift that
licence restriction at the same time. **This may be the highest
return-on-investment job left, and it takes one morning.**

---

## 7. What NOT to do on this trip

- **No writing code in the field.** If something breaks, note it and fix it at
  home. Compiling and flashing in a forest burns the trip.
- **No changing thresholds by hand in the field.** Change them and the first
  half of the trip is no longer comparable with the second.
- **No playing reference calls through the speaker on this trip.** That is a
  separate job; mixed into the same recording as live song it ruins both
  measurements.
