# SumAlign (Norka DSP)

Phase alignment for multi-mic sources: kick in/out, snare top/bottom vs overheads, DI + amp, two mics on a cab.
Put it on the tracks you want to line up and dial in **time**, **phase** and **polarity** against a reference.
The meter shows how well this track sums with the reference.

**Status: Phase 1 (manual core).** One-click ALIGN comes in Phase 2 (sidechain, one pair at a time) and Phase 3
(every instance in the session finds the others). The full plan is in the Plugin Creation project (`phase-align-plan.md`).

## Install (easiest)

Download **SumAlign-Installer.pkg** from the latest release: https://github.com/Harencorey/sumalign/releases/tag/latest
It isn't Apple-notarized yet, so the first time macOS blocks it: **System Settings > Privacy & Security > Open Anyway**
(or right-click the .pkg and choose **Open**). Rescan plug-ins and look for **Norka DSP > SumAlign**.
Every past version stays downloadable at https://github.com/Harencorey/sumalign/releases.

## Using it (Phase 1)

1. Insert SumAlign on the track you want to move (e.g. the overheads, or kick out).
2. Route the **reference** (e.g. the snare top or kick in) into SumAlign's **sidechain**. Don't put SumAlign on the
   reference track for now (or leave it at its defaults: Delay 0, Phase 0, Polarity off).
3. Play the part. Move **Delay**, then try **Polarity**, then the **Phase** knob for the highest **SUM vs REFERENCE**
   number. Then listen: the meter gets you close, your ears decide.

## Controls

| Control | What it does |
|---|---|
| **Delay** | Moves the track later (+) or earlier (-), up to 10 ms each way, in fractions of a sample. The line under it shows the same move in samples, cm and inches (343 m/s). Shift-click-drag = fine; double-click = 0; click the number to type a value. |
| **Nudge** | Moves the delay by 1 or 0.1 sample. **0** resets it. Whole-sample values are a plain sample copy (no interpolation at all). |
| **Phase** | Rotates the phase, like the Little Labs IBP: 0 = untouched; the angle is exact at the Phase Center. It's a 1st-order all-pass: the level never changes, the phase goes from 0 at the bottom of the spectrum towards 180 at the top (see the curve). 0 is bit-identical to off. Shift-drag for fine moves. |
| **Center** | The Phase Center: the frequency the Phase knob's angle applies to (20 Hz - 8 kHz). Low for kick/bass/toms, higher for snare, guitars, vocals. |
| **Invert** | Polarity flip. Switching ramps through zero over 10 ms, so it never clicks. |
| **SUM vs REFERENCE** | Correlation of this track's output with the sidechain, -1 (cancelling) .. +1 (summing). Weighted towards the mids and highs (a +6 dB/oct tilt), where misalignment shows up as comb filtering; broadband correlation on a kick or bass would barely move. Averaged over ~300 ms. |

## How it works

```
in -> fractional delay (64-tap Kaiser-windowed sinc) -> phase rotation (1st-order all-pass) -> polarity -> out
sidechain -> fixed latency -> correlation meter
```

- **Latency:** fixed at 10 ms + 31 samples (511 samples at 48 kHz), the same on every instance and every setting, reported
  to the host. That's what lets Delay go negative (move a track earlier). Bypass keeps the same latency.
- **Delay accuracy:** fractional delays land within -100 dB of the ideal delayed signal up to 16 kHz at 44.1 kHz.
  Moving the delay crossfades between the old and new setting over 10 ms (no zipper noise, no pitch bend).
- **Phase:** the bilinear all-pass H(z) = (a + z^-1)/(1 + a z^-1), with `a` set so the shift at the Phase Center is exactly
  the knob's angle (measured within 0.003 deg). It fades in/out over 20 ms and glides while you turn it.
- **Real-time safe:** no allocations, locks or I/O on the audio thread (tested). Mono and stereo, 44.1 - 192 kHz.

## Tests

- `tests/test_engine.cpp`:
  - latency, and Delay 0 = a bit-exact delayed copy (= bypass)
  - whole-sample delays exact across the full range
  - fractional accuracy against analytic sines at 6 sample rates
  - a null test: a lagged, polarity-flipped copy aligned back cancels to below -85 dB (measured -92 to -102 dB)
  - polarity: exact, click-free
  - phase angle exact at the centre and flat magnitude everywhere; phase 0 bit-identical, also after being on
  - delay/phase moves click-free
  - identical output at any block size
  - correlation meter readings (incl. a bass-heavy source 1.3 ms late: 0.05 vs 1.00 aligned)
  - no allocations
  - silence and an extreme impulse
- `tests/test_processor.cpp`:
  - bus layouts
  - reported latency and impulse timing in normal / bypassed / shifted processing at 44.1 - 192 kHz
  - the sidechain driving the meter
  - session save/load and typed values
  - the editor opening

## Build it yourself (Mac)

```bash
bash build_mac.sh                   # downloads JUCE 8.0.15 automatically
# or: bash build_mac.sh ~/path/to/JUCE
```

Needs the Xcode command line tools and CMake (`brew install cmake`). It builds a universal **AU, VST3 and Standalone**,
installs the plug-ins into `~/Library/Audio/Plug-Ins/`, and runs the tests. Logic: `auval -v aufx SmAl Nrka`.

Every push builds on a GitHub macOS runner (`.github/workflows/build-mac.yml`), the same pipeline as Glow and The Gate:
tests, a universal-binary check, **SumAlign-Installer.pkg**, a test install, Apple `auval` and pluginval at strictness 10,
then the **latest** release plus a permanent version release. Other branches publish a `test-<branch>` pre-release.

## Files

- `Source/AlignCore.h`: the whole audio path (plain C++, shared by the plug-in and the tests).
- `Source/Params.h`: parameter IDs (don't rename after release: it breaks sessions). Plug-in code `SmAl`, manufacturer `Nrka`: keep forever.
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`: JUCE wrapper and UI.
- `tools/Snapshot.cpp`: renders the real editor to a PNG (`-DSA_BUILD_TOOLS=ON`).
