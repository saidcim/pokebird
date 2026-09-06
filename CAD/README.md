# CAD

**There is no enclosure in this project yet.** The device is a bare Waveshare
RP2350-Touch-LCD-3.49 module with a LiPo cell attached; that is what went to
Belgrad Forest for the field test recorded in
[`docs/field-test/`](../docs/field-test/), and it is what the photographs in
the README show.

This folder exists so that the gap is stated rather than left for a reader to
discover. It is not a placeholder for work that was quietly skipped: no case
was designed, so no `.STEP` file exists to publish.

## What an enclosure would have to respect

Anyone picking this up — including future me — needs these constraints, all of
which come from the module itself and from the recognition path:

- **The board.** 3.49 in diagonal display, 172 × 640 pixels, used in landscape.
  Overall dimensions and mounting-hole positions are in Waveshare's mechanical
  drawing: <https://www.waveshare.com/wiki/RP2350-Touch-LCD-3.49>
- **The microphone port must stay open.** The analogue MEMS microphone is on
  the board. A sealed case turns it into a low-pass filter and the 7–9 kHz band
  this project deliberately chose 24 kHz sampling to keep — Goldcrest,
  treecreepers, some warblers — is exactly what a covered port loses first.
- **Do not couple the case to the speaker.** The class-D amplifier drives a
  small speaker; a rigid mount that touches both it and the microphone side
  gives the device a mechanical path to hear itself.
- **Leave the power button and the USB-C port reachable.** The board's own
  button is the only wake/sleep control; USB-C is both charging and the serial
  link the field logger depends on.
- **Battery.** A LiPo cell with an MX1.25 connector; the pack sits behind the
  board and needs somewhere flat to live without pressing on the antenna area.

## Status

Open. If you build a case, a `.STEP` assembly dropped in this folder is the
right place for it.
