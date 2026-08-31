# PokeBird

A handheld, **fully offline** bird-song species identifier running on a
Waveshare **RP2350-Touch-LCD-3.49**. It listens through the board's onboard
microphone and names the bird on its own screen — no phone, no internet, no
cloud. Scoped to the ~178 bird species of Istanbul.

Full architecture: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (Turkish) · Turkish README: [`README.tr.md`](README.tr.md)

## Why it is interesting

BirdNET, the reference model for this task, has ~6000 classes and weighs tens of
megabytes. This board has **520 KB of SRAM, no PSRAM**, and two Cortex-M33 cores
at 150 MHz. Running BirdNET here is physically impossible. Narrowing the problem
to one city cuts the output space 34× *and* increases the training data per
class, so the small model ends up both smaller and more accurate than a shrunken
global one would be.

## Measured results

Stage-1 binary network (bird / not-bird), INT8, held-out test set:

```
accuracy 96.65%   bird recall 97.90%   negative specificity 86.58%
7,217 parameters · 1.44 MMAC/window · 20.9 KB
```

Stage-2 species network (178 species + "unknown"), INT8, held-out test set:

```
per 3 s window       top-1 58.07%   top-3 74.82%
8-window aggregate   top-1 70.40%   top-3 82.20%
209,107 parameters · 6.6 MMAC/window · 270 KB
```

INT8 quantisation cost: +0.02 points for the species net, +0.05 for the binary
net — i.e. none.

The on-screen confidence threshold was **measured separately from accuracy**,
because they are different questions:

```
threshold   coverage   precision
   0.60       35.6%      85.4%     <- write a species name
   0.35       59.7%      71.7%     <- erase it
```

Measured TFLM tensor arena on device: **110,436 bytes**.

## Hardware

| | |
|---|---|
| MCU | RP2350**B** — 2× Cortex-M33 @ 150 MHz, FPU + DSP |
| Memory | 520 KB SRAM (**no PSRAM**), 16 MB flash |
| Display | 172×640 IPS, AXS15231B, QSPI + touch |
| Audio | ES8311 codec, onboard analog MEMS mic, NS4150B speaker amp |
| Storage | microSD |

The pin map lives in [`src/board_config.h`](src/board_config.h) and was verified
net-by-net against Waveshare's official schematic. **Every hardware number comes
from there.**

## How it works

```
┌─ Core 1 (audio + AI, real time) ─────────────────────────────┐
│  I2S/PIO+DMA → ring buffer → incremental mel features        │
│      → gate (VAD) → stage-1 binary net → stage-2 species net │
│      → temporal aggregation + hysteresis                     │
└──────────────────── FIFO (detection events) ─────────────────┘
┌─ Core 0 (UI + storage, not real time) ───────────────────────┐
│  LVGL → AXS15231B QSPI+DMA │ touch │ SD card │ battery        │
└──────────────────────────────────────────────────────────────┘
```

Audio capture cannot drop a single sample; LVGL redraws and SD writes can block
for tens of milliseconds. Splitting the cores guarantees the UI never disturbs
the audio path.

**Signal path:** 24 kHz / 16-bit mono · 3.0 s window, 1.0 s hop · 512-point FFT,
Hann, 384-sample hop → 187 frames · 64 mel bands, 150 Hz – 11.5 kHz · log-mel,
per-window normalisation, INT8.

**The memory trick:** three seconds of raw audio would be 144 KB, so it is never
stored. Mel frames are computed incrementally into a 64×187 INT8 ring buffer
(12 KB); only ~0.5 s of raw audio (24 KB) is kept for optional WAV capture and
noise-floor estimation. That saves ~130 KB of SRAM.

**No full framebuffer** — 172×640 in RGB565 is 220 KB, 42% of SRAM. LVGL runs in
partial-render mode over two small buffers and a hand-written QSPI PIO driver.

## Build

Requirements: CMake ≥ 3.13, Ninja, ARM GCC (PlatformIO's
`toolchain-rp2040-earlephilhower`, GCC 14.3, is found automatically), and the
Pico SDK.

```bash
git clone -b 2.3.0 --depth 1 https://github.com/raspberrypi/pico-sdk.git third_party/pico-sdk
git -C third_party/pico-sdk submodule update --init --depth 1 lib/tinyusb

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/pokebird.uf2`. Paths are resolved by
[`cmake/toolchain.cmake`](cmake/toolchain.cmake) — no environment variables
needed. Override with `-DPICO_SDK_PATH=...` / `-DPICO_TOOLCHAIN_PATH=...` if your
setup differs.

## Flashing

1. Hold **BOOT** and **RESET** together
2. Release **RESET** first, then **BOOT**
3. Copy `build/pokebird.uf2` to the drive that appears

## Training pipeline (PC side, `tools/`)

1. Species list: eBird Istanbul (TR-34) ∩ Avibase, filtered by Xeno-canto coverage
2. Recordings from the Xeno-canto API (CC-licensed, quality A/B first)
3. **BirdNET as segmenter and teacher** — finds which 3 s slices actually contain
   the target species, and its output probabilities are kept as soft labels
4. **Istanbul-specific negative mining** — traffic, horns, the call to prayer,
   ferry horns, speech, dogs, wind, rain, construction. Skip this and the device
   is unusable in a real city.
5. Augmentation: time shift, pitch/tempo, noise mixing at varied SNR, SpecAugment
6. Distillation training against hard labels + BirdNET soft outputs, focal loss
7. INT8 post-training quantisation, accuracy reported before and after
8. Conversion to C arrays under `models/`
9. **On-device verification set** — labelled windows and the PC's reference logits
   are compiled into the firmware, so the device proves its inference matches the
   PC. This catches every "works on my laptop, not on the board" bug.

## Status

| Milestone | |
|---|---|
| M0 — project skeleton, build chain | ✅ |
| M1 — microphone bring-up + SNR measurement | ✅ |
| M2 — display + LVGL + live spectrogram | ✅ |
| M3 — DSP pipeline (mel + gate), verified against a Python reference | ✅ |
| M4 — data pipeline, species list | ✅ |
| M5 — model training + distillation + INT8 | ✅ |
| M6 — TFLM integration, real-time inference on core 1 | ✅ |
| M7 — post-processing, full UI, logging | in progress |
| M8 — field calibration in Istanbul | |

The device currently listens, recognises, and writes the species name on screen
in real time.

## Honest limitations

- Test-set accuracy is not field accuracy. Real city noise will lower it; M8 is
  the field-calibration milestone.
- Some species are near-indistinguishable (certain warblers, certain gulls). The
  plan is to merge these into species *groups* rather than give false precision.
- The UI shows top-3 with confidence and says "not sure" below threshold. That is
  a design decision: a device that confidently says one wrong name is worse than
  one that offers three candidates.

## Repository layout

```
boards/     Pico SDK board definition (no RP2350B header ships with the SDK)
cmake/      Toolchain helpers
src/        Device code — board_config.h, hal/, dsp/, ai/, ui/
models/     Quantised INT8 models as C arrays + accuracy records
tools/      PC side: data collection, training, conversion (Python)
test/       Host-side DSP unit tests
```

Source identifiers and comments are in Turkish. The Turkish README is kept at
[`README.tr.md`](README.tr.md).

## Licence note

BirdNET is used as the teacher during training and is distributed under
**CC BY-NC-SA 4.0**. A distilled model may count as a derivative work, which
restricts commercial use. Personal and research use is fine. See §6 of the
architecture document for detail and for the commercial-use path (segmentation
only, no distillation). Xeno-canto recordings carry per-recording CC licences;
an `ATTRIBUTION.md` is generated automatically.

## References

- [RP2350-Touch-LCD-3.49 — Waveshare Wiki](https://www.waveshare.com/wiki/RP2350-Touch-LCD-3.49)
- [BirdNET-Analyzer](https://github.com/birdnet-team/BirdNET-Analyzer) — teacher model
- [eBird — Istanbul (TR-34)](https://ebird.org/region/TR-34) — species list and monthly frequencies
