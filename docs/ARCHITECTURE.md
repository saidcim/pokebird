# PokeBird — Istanbul Bird Song Recognition Device: Architectural Groundwork

> This is the design document written before any code existed. It records the
> reasoning the project was built on, and the numbers in it are the budgets
> and expectations of that moment, not measurements. Where the built device
> differs — 178 species rather than ~110, the firmware under `firmware/` —
> the measured figures live in `models/*.txt` and the repository README.

## Context

**Goal:** a pocket device on the Waveshare RP2350-Touch-LCD-3.49 that works
with no internet connection and recognises a bird species from its song. What
BirdNET does, narrowed to a single city (Istanbul) and squeezed into a 150 MHz
microcontroller.

**Why the narrowing is necessary:** BirdNET's GLOBAL 6K model has ~6000
classes, a 48 kHz input and a network tens of megabytes in size. This device
has 520 KB of SRAM and two cores at 150 MHz — running BirdNET as it is is
physically impossible. A list of ~110 species specific to Istanbul both cuts
the class count by a factor of 50 and, by increasing the training data per
class, raises the accuracy of a small model.

**What this document is:** the foundation to build on. The hardware facts
(verified from the schematic), the memory and compute budget, the signal
processing and model architecture, the data pipeline, the interface and the
staged delivery plan.

**Project state:** `C:\Users\hp\Downloads\pokebird` is an empty git repository.
We are starting from scratch.

**Decisions locked in (with the user's approval):**
| Decision | Choice |
|---|---|
| Species scope | ~110 species (resident + regular breeders + common migrants) |
| Use | Handheld, instant recognition; the screen stays on |
| Software stack | Pico SDK + CMake (C/C++) |
| Model training | BirdNET as teacher -> distilled into a small model |

---

## 1. The Hardware Foundation (verified from the schematic)

Extracted by working through the Waveshare schematic
(`RP2350-Touch-LCD-3.49.pdf`). **This table is the source of the project's
`board_config.h`.**

| GPIO | Signal | Note |
|---|---|---|
| 0 | `NS_MODE` | NS4150B speaker amplifier mode/enable |
| 1 | `I2S_DSDIN` | MCU -> codec (DAC, playback) |
| 2 | `I2S_DSOUT` | codec -> MCU (**ADC, microphone data**) |
| 3 | `I2S_MCLK` | to be generated with PIO |
| 4 | `I2S_SCLK` | BCLK |
| 5 | `I2S_LRCK` | WS |
| 6 / 7 | `SDA` / `SCL` | **Shared I2C**: ES8311 codec + QMI8658 IMU + PCF85063 RTC |
| 8 / 9 | `IMU_INT1` / `IMU_INT2` | *will not be used* |
| 10 | `RTC_INT` | *will not be used* |
| 11 | `TP_INT` | touch interrupt |
| **12–19** | **free** | brought out to header P3 (8 pins) |
| 20–25 | `LCD_SCL`, `LCD_D0..D3`, `LCD_CS` | AXS15231B QSPI |
| 26–31 | `SD_SCLK`, `SD_MOSI`, `SD_MISO`, `SD_D1`, `SD_D2`, `SD_CS` | 4-bit SDIO is also possible |
| 32 / 33 | `TP_SDA` / `TP_SCL` | the touchscreen's **separate** I2C bus |
| 34 / 35 / 36 / 37 | `LCD_RST`, `LCD_TE`, `LCD_BL`, `BL_EN` | |
| 38 / 39 | `SYS_OUT` / `SYS_EN` | power latch |
| 40 | `BAT_ADC` | battery voltage |
| **41–47** | **free** | header J3/J6 (7 pins) |

**Critical hardware facts:**
- **RP2350B**: 2x Cortex-M33 @ 150 MHz, FPU + the DSP extension (so CMSIS-DSP/NN
  are available).
- **520 KB SRAM, 16 MB flash (PY25Q128HA). NO PSRAM.** — this is the hardest
  constraint.
- **The microphone is on the board**: an analogue MEMS microphone (`MIC1`) into
  the ES8311's `MIC1P/MIC1N` input. No external microphone is needed.
- **There is a speaker output**: an NS4150B class-D amplifier plus an MX1.25
  connector (H1). Playing a reference bird call is possible.
- **Peripherals deliberately left unused:** the IMU (QMI8658) and the RTC
  (PCF85063). The IMU contributes nothing to this project and only adds
  complexity. The RTC has no battery backup — the device loses the time when
  it is switched off, so it is not a reliable date source (for date handling
  see §4). Waking and sleeping the screen will be done with the board's own
  power button.
- **The screen is 172x640** — narrow and long. Turned landscape it is a
  640x172 "strip". A full frame buffer in RGB565 is **220 KB**, 42% of the
  SRAM. **We will not use a full framebuffer** (see §5).

**Existing code to reuse (we will not write it from scratch):**
`github.com/waveshareteam/RP2350-Touch-LCD-3.5` has, under
`examples/C/03_ES8311/`, a working **ES8311 driver plus PIO-based I2S**;
inside it are `read_pio` (microphone capture), `mclk_pio` (MCLK generation),
the microphone gain setting and the 16-bit / 24 kHz configuration. Because it
belongs to a sibling board **only the pin definitions change** (the
`PICO_AUDIO_*` macros -> GPIO 1–5 above). That solves the riskiest part of the
project off the shelf.

---

## 2. System Architecture

```
+- Core 1 (audio + AI, real time) ------------------------------+
|  I2S/PIO+DMA -> ring buffer -> mel feature extraction         |
|      -> gate (VAD) -> stage-1 binary net -> stage-2 species   |
|      -> temporal voting + seasonal prior                      |
+--------------------- FIFO (detection events) -----------------+
+- Core 0 (interface + storage, not real time) -----------------+
|  LVGL -> AXS15231B QSPI+DMA | touch | SD card | battery | pwr |
+---------------------------------------------------------------+
```

**Why this division:** audio capture cannot miss a single sample. LVGL's draw
loop and SD card writes can block for tens of milliseconds. Separating the two
cores guarantees that the interface never disturbs the audio path. There is
only one direction between the cores: Core 1 -> Core 0, and only detection
events (small structs), over the `pico_multicore` FIFO plus a lock-free ring
buffer.

**Layers:**
| Layer | Responsibility |
|---|---|
| `hal/` | Pico SDK wrappers: i2s, i2c, qspi_lcd, touch, sd, power |
| `dsp/` | ring buffer, windowing, RFFT, mel filter bank, log + normalisation |
| `ml/` | the TFLM runner, the gate logic, two-stage inference, post-processing |
| `data/` | species table, seasonal priors, detection log, life list |
| `ui/` | LVGL screens, theme, fonts |
| `app/` | task loops, the state machine, persisting the settings |

---

## 3. The Audio and Signal Processing Path

**Sample rate: 24 kHz** (not 16 kHz). The reason: 16 kHz means an 8 kHz
Nyquist, and the Goldcrest (*Regulus regulus*), the treecreepers (*Certhia*)
and some warblers found in Istanbul sing in the 7–9 kHz band; an 8 kHz ceiling
clips them. 24 kHz gives a 12 kHz Nyquist and covers all of them. On top of
that the Waveshare driver's default is 24 kHz — no extra work.

| Parameter | Value |
|---|---|
| Sampling | 24 kHz, 16-bit, mono |
| Analysis window | 3.0 s, with a 1.0 s step (66% overlap) |
| FFT | 512 points, Hann |
| Hop | 384 samples (16 ms) -> 187 frames in 3 s |
| Mel bands | 64, over 150 Hz – 11.5 kHz |
| Output | log-mel, mean/variance normalised within the window, int8 |

**The critical memory trick:** we will not hold 3 seconds of raw audio (that
would be 144 KB). The mel frames are computed **incrementally** and kept in a
64x187 ring buffer (int8 -> 12 KB). For raw audio only a short ~0.5 s ring
(24 KB) is kept — for optional WAV recording and noise estimation. That single
decision saves about 130 KB of SRAM.

**The gate — a cheap filter that runs continuously:** the energy in the
2–10 kHz band plus spectral flux, relative to an adaptive noise floor. Since
the device will be in a "there is nothing here" state most of the time in a
city, this gate means the heavy work does not run at all more than 90% of the
time -> battery life.

---

## 4. Model Architecture: a Three-Stage Pyramid

A graded structure rather than one large 110-class network. The reason: the
core finding of the DrongoNet work
([arXiv 2607.19721](https://arxiv.org/html/2607.19721)) is that "is there a
sound" and "which species" cost wildly different amounts — asking the cheap
question first raises both accuracy and battery life.

| Stage | Job | Size | When it runs |
|---|---|---|---|
| **0. Gate** | energy + spectral flux | ~0 KB (DSP) | continuously |
| **1. Binary net** | bird song or not | ~15 KB int8 | when the gate fires |
| **2. Species net** | 110 species + "unknown" | ~300–400 KB int8 | if stage 1 says "bird" |
| **3. Voting** | temporal voting + seasonal prior | ~2 KB table | on every detection |

**The stage-2 network:** a depthwise-separable convolutional CNN — the
narrowed-MobileNet idea. The input is 64x187 int8. The target budget is
**<=30 MMAC per window**. With CMSIS-NN on the M33 at 150 MHz that comes to
~0.3–0.5 s, which fits comfortably inside a 1 s window step.

**Stage 3 — the seasonal prior (small work, large gain):** Istanbul's monthly
species frequencies are taken from eBird and quantised into a 110x12 table
(1.3 KB). A month-based log prior is added to the logits, so a bee-eater
prediction in January is suppressed automatically. **An important safeguard:**
the prior's effect is capped (say +-2.0 logits) and can be turned off in the
settings — otherwise the device could never report a genuine rare sighting.

**Where the date comes from — we do not use the RTC.** The board's PCF85063
RTC has no battery backup, so the device loses the time when it is switched
off; it is an unreliable source. Instead:
- a single-screen **date confirmation** step at startup: the last known date,
  stored in LittleFS, comes up as the default, and the user either presses
  "Confirm" or changes it on a wheel. One touch in daily use.
- **only the month** is needed for the prior — day and hour precision does not
  matter, so being a day or two out breaks nothing.
- the within-session clock counts from startup on the MCU timer (enough for
  the timestamps in the log). The current date is written to LittleFS on
  shutdown.
- the user can skip entering a date entirely -> the seasonal prior is disabled
  for that session and the device still works.

**Temporal voting:** an exponential moving average of the softmax plus a
"k of the last n windows above the threshold" rule. It removes most of the
false positives caused by noise in a single window.

**A realistic accuracy expectation (worth saying up front):** on clean
single-species recordings, a top-1 of 65–75% and a top-3 of 85–90% is a
reasonable target. In a real urban environment (traffic, the call to prayer,
gulls, human voices) that drops noticeably. That is why the interface will
show **not a single answer but the top 3 predictions with confidence
percentages**, and will say plainly "not sure" at low confidence. We treat
this as a design decision, not a defect.

---

## 5. The Memory Budget (520 KB SRAM)

| Component | Estimate |
|---|---|
| TFLM tensor arena (stage 2) | 180 KB |
| LVGL draw buffers (2 x 640x20 px RGB565) | 26 KB |
| mel ring buffer | 12 KB |
| raw audio ring buffer (0.5 s) | 24 KB |
| I2S DMA buffers | 8 KB |
| FatFS + SD buffers | 10 KB |
| species table + log + settings | 20 KB |
| stacks (2 cores) + heap + LVGL objects | 80 KB |
| **Total** | **~360 KB** — about 160 KB of headroom |

**The display strategy (NO full framebuffer):** LVGL runs in partial render
mode with two small buffers; as one finishes drawing it is pushed to the QSPI
by DMA while the other is filled. The `LCD_TE` (tearing effect) signal is used
to avoid tearing. The scrolling spectrogram is drawn not with an LVGL canvas
but by a direct column-push routine — only one pixel of new column goes to the
QSPI per frame, which is very cheap.

**The flash layout (16 MB):**
| Region | Size |
|---|---|
| firmware | ~900 KB |
| models (stages 1 + 2) | ~450 KB |
| species metadata (names, monthly prior) | ~60 KB |
| fonts + icons | ~400 KB |
| LittleFS (settings, last known date, life list, log) | 2 MB |
| free / OTA headroom | ~12 MB |

**The SD card (optional but recommended):** bird photographs, reference
recordings (to play through the speaker), an archive of raw WAV recordings, a
CSV detection log. The device works fully without an SD card — only the
photographs and playback are disabled.

---

## 6. The Data and Training Pipeline (the PC side)

Python under `tools/`; it never touches the device code.

1. **Finalising the species list** — the eBird Istanbul (TR-34) taxonomy is
   intersected with the Avibase checklist; species that make no sound, cannot
   be told apart, or have fewer than 30 recordings on Xeno-canto are dropped
   -> a final ~110 species. Output: `data/species_istanbul.csv` (scientific
   name, Turkish name, English name, eBird code).
2. **Collecting recordings** — CC-licensed recordings are downloaded from the
   Xeno-canto API, with quality A/B prioritised.
3. **Automatic segmentation and soft labelling with BirdNET** —
   BirdNET-Analyzer is run over every recording to find which 3-second slices
   really contain the target species (most of a Xeno-canto recording is
   silence and background species, so this step improves data quality
   dramatically). BirdNET's output probabilities are kept as the teacher
   signal.
4. **Negative mining (specific to Istanbul, critical)** — traffic, car horns,
   the call to prayer, ferry horns, human speech, dogs and cats, wind, rain,
   construction. These become both stage 1's negative class and stage 2's
   "unknown" class. **Skip this step and the device is unusable in the field**
   — it will take city noise for a bird continuously.
5. **Augmentation** — time shifting, pitch/tempo, mixing in noise (with the
   negatives above, at various SNRs), SpecAugment, room/distance simulation.
6. **Training with distillation** — the small CNN is trained on both the true
   labels and BirdNET's soft outputs. Focal loss for the class imbalance.
7. **Quantisation (INT8) + verification** — TFLite post-training quantisation
   with a representative data set. The accuracy difference before and after
   quantisation is reported.
8. **Conversion to a C array** — along the lines of `xxd -i`, into `models/`.
9. **An on-device validation set** — a test mode that plays a WAV from the SD
   card and compares the device's own output against the reference on the PC.
   This catches the "works on the PC, does not work on the device" class of
   bug.

**A licence warning (worth knowing now):** the BirdNET models are distributed
under **CC BY-NC-SA 4.0**. A model distilled using BirdNET as its teacher may
count as a derivative work — which **restricts commercial use** and can create
a share-alike obligation. For personal, hobby or research use there is no
problem. If commercialisation is ever considered, a variant is needed in which
step 3 is used only for segmentation and the soft labels (the distillation)
are skipped. Xeno-canto recordings also carry different CC licences per
recording; an attribution file (`ATTRIBUTION.md`) will be generated
automatically.

---

## 7. Interface Design (640x172 landscape)

The screen is an unusual strip — we will treat that as an advantage rather
than a defect: a fixed "identity card" on the left, a scrolling spectrogram on
the right.

**Screen 1 — Listening (the main screen)**
```
+--------------------+---------------------------------------+
|  EUROPEAN ROBIN    |  ..:::##|##::.  (scrolling spectrogram)|
|  Erithacus rub.    |                                       |
|  * 87%   14:32     |  =======...  level                    |
|  2. Sparrow 6%     |                                       |
+--------------------+---------------------------------------+
```
At low confidence the card switches to "Listening…" or "Not sure — 3
candidates".

**Screen 2 — Detection detail:** a bird photograph from the SD card, the
names, the confidence, the time, and a "> Play the reference call" button (the
speaker). *Note: listening is paused during playback — otherwise the device
hears itself.*

**Screen 3 — Log:** today's detections (time + species + confidence), day-list
and life-list tabs, CSV export to the SD card.

**Screen 4 — Settings:** the sensitivity threshold, the seasonal prior on/off,
microphone gain, screen brightness, language, raw WAV recording on/off, the
date.

**Screen 0 — Date confirmation (at startup):** a single line: `Today: 31 July
2026` + [Confirm] [Change] [Skip]. The default is the last known date from
LittleFS. Its purpose is the seasonal prior (§4); "Skip" disables the prior for
that session. One touch in daily use.

Moving between screens is by a horizontal swipe. Turning the screen on and off
is done with the board's own power button — there is no extra wake mechanism.

---

## 8. Repository Layout

```
pokebird/
├─ CMakeLists.txt              # PICO_BOARD=pico2, PICO_PLATFORM=rp2350
├─ firmware/
│  ├─ src/
│  │  ├─ main.c                # the core0 entry point; it starts core1
│  │  ├─ board_config.h        # the pin table from §1 - the ONE source of truth
│  │  ├─ hal/                  # i2s_mic, es8311, axs15231b, touch, sdcard, power
│  │  ├─ dsp/                  # ringbuf, window, mel, gate
│  │  ├─ ai/                   # the TFLM runner, the two-stage pipeline, the
│  │  │                        # generated class table and validation sets
│  │  └─ ui/                   # the screens, the theme, the generated fonts
│  ├─ boards/                  # the board definition
│  └─ test/                    # host-side DSP unit tests (dsp_test)
├─ models/                     # species_net_int8.h, binary_net_int8.h, the reports
├─ third_party/                # pico-tflmicro, CMSIS-DSP/NN, lvgl, FatFS
├─ tools/                      # Python: data collection, training, quantisation
└─ docs/                       # this document, the field-test protocol and reports
```

---

## 9. Staged Delivery

Every stage can be verified on its own. **The order is deliberate: the
riskiest part (the microphone) comes first.**

| # | Stage | Output / verification |
|---|---|---|
| **M0** | Skeleton: CMake + Pico SDK, a blinking LED, a USB serial log | the board can be programmed |
| **M1** | **Microphone bring-up** (the Waveshare ES8311 example, adapted) | records 5 s of audio and writes a WAV to the SD card; listened to on the PC and its SNR measured. **On-board EMI / display noise is measured here.** |
| **M2** | Display + touch + LVGL, partial render, a live spectrogram | the microphone's audio scrolls across the screen |
| **M3** | The DSP path: mel + gate, bit-compatibility against host-side tests | the mel on the device is ~the same as the mel in Python |
| **M4** | The data pipeline + finalising the species list (PC) | `species_istanbul.csv` plus a downloaded and segmented data set |
| **M5** | Model training + distillation + INT8 + an accuracy report on the PC | a confusion matrix, top-1/top-3 metrics |
| **M6** | TFLM integration, two-stage inference, on core1 | the device names a species in real time; latency is measured |
| **M7** | Post-processing (prior + voting), the date screen, the full interface, the log, the battery | a device usable in the field |
| **M8** | Field calibration: setting the thresholds against real Istanbul recordings | an acceptable false-positive rate |

---

## 10. Risks and Countermeasures

| Risk | Impact | Countermeasure |
|---|---|---|
| **The on-board microphone's SNR is bad** (display / QSPI / switching-supply noise) | could end the project | **Measure it in M1.** If it is bad: shift the LCD brightness PWM frequency, reduce screen refresh while listening, and as a last resort add an external I2S MEMS microphone (INMP441/ICS-43434) on the free GPIOs (12–19) — the board supports it |
| The ES8311 pins differ from the 3.5 board | medium | defined in one place in `board_config.h`; the §1 table was verified against the schematic |
| The MCLK / master-slave clock configuration does not hold | medium | the ES8311 can use SCLK as MCLK (a register option); the PIO MCLK already works in the Waveshare example |
| Accuracy over 110 classes is below expectations | likely | the interface shows the top 3 from the start; a graceful fallback: cutting the list to 60 and retraining is only a model and CSV change, the code does not move |
| Some species are nearly indistinguishable by sound (some warblers, some gulls) | medium | those species are merged into a "species group" (e.g. "Herring/Black-headed gull group") — better than false certainty |
| The tensor arena does not fit in SRAM | medium | the model architecture is designed to the budget (§4); an early stride-2 keeps the activations small; the arena size is measured in M5 and verified before M6 |
| The BirdNET licence restricts commercial use | low (for a hobby) | documented in §6; a distillation-free variant is defined for a commercial route |

---

## 11. The Verification Approach

- **Host-side unit tests** (`firmware/test/`): the DSP functions (windowing,
  RFFT, the mel filter bank) are compiled on the PC and compared against the
  Python reference. Debugging a DSP bug on a microcontroller is very
  expensive — these tests save time.
- **The on-device WAV test:** a test mode that feeds labelled validation clips
  from the SD card into the input of the path instead of the microphone. The
  device's output is compared against the reference on the PC -> it catches
  the "works on the PC, does not work on the device" bugs.
- **Latency and memory measurement:** the time of each stage (mel, gate, stage
  1, stage 2) is reported over USB serial; TFLM arena usage is logged with
  `arena_used_bytes()`.
- **The field test (M8):** recording plus a comparison against the device's
  output at eBird hotspots such as Belgrad Forest, Validebağ Grove or
  Büyükçekmece. Running BirdNET on a phone at the same time and measuring how
  often the two agree is a practical, fast reference.
- **Battery life:** measured in a continuous-listening scenario with the
  screen on; the target is >=4 hours.

---

## Sources

- [RP2350-Touch-LCD-3.49 — Waveshare Wiki](https://www.waveshare.com/wiki/RP2350-Touch-LCD-3.49) (the schematic PDF was worked through from here)
- [waveshareteam/RP2350-Touch-LCD-3.5](https://github.com/waveshareteam/RP2350-Touch-LCD-3.5) — the ES8311 + PIO I2S driver to reuse
- [DrongoNet: Ultra-Compact CNN Architectures for Bird Audio Detection on Microcontrollers](https://arxiv.org/html/2607.19721) — the reasoning for the staged architecture and the memory budget
- [eBird — Istanbul (TR-34)](https://ebird.org/region/TR-34) — the species list and monthly frequencies
- [BirdNET-Analyzer](https://github.com/birdnet-team/BirdNET-Analyzer) — the teacher model
