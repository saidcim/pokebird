# Pokebird

A portable device that listens bird sounds and identifies their species. It runs on
a rp2350 microcontroller with no internet connection. and it knows the 178 bird 
species that live and can be heard in istanbul

## What it does
You press the button, hold it up, and it listens. When it is reasonably sure, the
species name appears. When it isn't, it says so instead of guessing

## How it works
I could not run BirdNET—which serves the same purpose—on this device, so I used it
only as a reference; I narrowed the scope from approximately 6,000 species down to
the 178 species found in my city(Istanbul/Turkey). By reducing the number of species,
I was able to increase the training data used for each one, resulting in a system 
that is both smaller and more accurate.

## Results

Measured on a held-out test set. The raw output files are in `models/`.

The first network decides whether a sound is a bird at all:

    accuracy 96.65%    bird recall 97.90%    20.9 KB

The second one decides which bird:

    top-1 58.07% per 3-second window
    top-1 70.40%, top-3 82.20% when 8 windows vote together
    270 KB

Listening for eight seconds instead of three is worth twelve points, and it costs
nothing extra in memory.


## Hardware
I used Waveshare [RP2350-Touch-LCD-3.49](https://www.waveshare.com/rp2350-touch-lcd-3.49.htm?srsltid=AfmBOopNxvXnEowwXBLtuLvmao28l89tuNQRQi_HE1xzq5fKwh1gDy0S) 
module for this project since it has a Built-in LCD, Onboard ES8311 audio codec, Microphone and TF card slot.

## How it works

I achieved a division of labor by assigning different tasks to two processor cores.

### Core 1 - Real time

    microphone - I2S/DMA -> ring buffer -> mel features
               -> gate -> binary net -> species net -> voting


### Core 2 - Not real tine

    LVGL screen · touch · SD card · battery

I achieved a division of labor by assigning different tasks to two processor cores. 
It was necessary to handle screen and audio capture on separate cores because the 
screen was generating noise.

### The memory trick

Audio comes every 3 seconds of it becomes one analysis window, Three seconds of
raw audio is 144 KB. On a chip with 520 KB and no PSRAM, that is most of the memory
budget spent before identification starts.

So the device never keeps it. Each mel frame is computed the moment the audio
arrives and written into a small ring buffer, 64x187 in INT8, 12 KB in total.
Raw audio is kept for only half a second, 24 KB, enough for the noise floor
estimate and 130 KB cheaper.

### No framebuffer

The screen is 172x640. A full RGB565 framebuffer would be 220 KB, over 40% of
SRAM. LVGL is cheaper it runs in partial render mode over two small
buffers, pushed out through a QSPI PIO driver I wrote by hand.

### The recognition pyramid

Asking "is that a bird" is much cheaper than asking "which bird," so the algorithm
asks the cheap question first.

| Stage | What it asks | Cost | Runs when |
|---|---|---|---|
| 0. Gate | Is anything happening? | free, plain DSP |
| 1. Binary net | Is it a bird? | 1.44 MMAC, 20.9 KB |
| 2. Species net | Which bird? | 6.6 MMAC, 270 KB |
| 3. Voting | Am I sure enough to say it? | free |

Each stage exists so the next one does not have to run. 

## How the model was trained

All of this happens on my PC, in `tools/`. None of it ships to the device.

The species list comes from eBird's Istanbul records, filtered down to species
that have enough recordings to learn from. The recordings come from Xeno-canto.
Xeno-canto files are mostly silence and background birds, so BirdNet is used to
find which three second slices actually contain the target species,I used BirdNet's own
output and used as a teacher

## Field test

I took it to Belgrad Forest, north of Istanbul, on 31 August 2026. I kept it on for the
whole walk and It succesfully named Common Chaffinch, Hooded Crow, European Robin and
woodpeckers correctly

## Setup
dependencies: CMake, Ninja, an ARM GCC toolchain and the Pico SDK.
```bash
git clone -b 2.3.0 --depth 1 https://github.com/raspberrypi/pico-sdk.git third_party/pico-sdk
git -C third_party/pico-sdk submodule update --init --depth 1 lib/tinyusb

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
1. After That "pokebird.uf2" file in /build folder will be created
2. To flash it, hold "boot" and "reset" button and release "reset" first, a drive
3. will show up copy the file onto the drive and thats it

## Licence

BirdNET is CC BY-NC-SA 4.0, and a model distilled from it may count as a
derivative work, so commercial use is restricted. Personal and research use is
fine.



