# SumAlign (Norka DSP)

Phase alignment for multi-mic sources: kick in/out, snare top/bottom vs overheads, DI + amp, two mics on a cab.
Put it on the tracks you want to line up and dial in **time**, **phase** and **polarity** against a reference.
The meter shows how well this track sums with the reference.

**Status: Phase 2 (ALIGN through the sidechain, one pair at a time).** Phase 3 makes it one click for the whole
session (every instance finds the others, no sidechain routing). The full plan is in the Plugin Creation project (`phase-align-plan.md`).

## Install (easiest)

Download **SumAlign-Installer.pkg** from the latest release: https://github.com/Harencorey/sumalign/releases/tag/latest
It isn't Apple-notarized yet, so the first time macOS blocks it: **System Settings > Privacy & Security > Open Anyway**
(or right-click the .pkg and choose **Open**). Rescan plug-ins and look for **Norka DSP > SumAlign**.
Every past version stays downloadable at https://github.com/Harencorey/sumalign/releases.

## Using it

1. Insert SumAlign on the track you want to move (e.g. the overheads, or kick out).
2. Route the **reference** (e.g. the snare top or kick in) into SumAlign's **sidechain**. Don't put SumAlign on the
   reference track for now (or leave it at its defaults: Delay 0, Phase 0, Polarity off).
3. Press **ALIGN** and play the song (the whole song is best; a few seconds of drums is usually enough). It shows
   progress while it listens and sets **Delay** and **Polarity** when it's sure. Press it again (FINISH) to stop early.
   If it isn't sure, it changes nothing and tells you why.
4. Optional: **Earlier / Later** jump to other points where the track also lines up (e.g. keep some of a room mic's
   distance while staying in phase). Then fine-tune by ear with **Phase**, and check the **SUM vs REFERENCE** meter.

## Controls

| Control | What it does |
|---|---|
| **Delay** | Moves the track later (+) or earlier (-), up to 30 ms each way (about 10 m: far enough for room mics), in fractions of a sample. The line under it shows the same move in samples, cm and inches (343 m/s). Shift-click-drag = fine; double-click = 0; click the number to type a value. |
| **Nudge** | Moves the delay by 1 or 0.1 sample. **0** resets it. Whole-sample values are a plain sample copy (no interpolation at all). |
| **Phase** | Rotates the phase, like the Little Labs IBP: 0 = untouched; the angle is exact at the Phase Center. It's a 1st-order all-pass: the level never changes, the phase goes from 0 at the bottom of the spectrum towards 180 at the top (see the curve). 0 is bit-identical to off. Shift-drag for fine moves. |
| **Center** | The Phase Center: the frequency the Phase knob's angle applies to (20 Hz - 8 kHz). Low for kick/bass/toms, higher for snare, guitars, vocals. |
| **Invert** | Polarity flip. Switching ramps through zero over 10 ms, so it never clicks. |
| **SUM vs REFERENCE** | Correlation of this track's output with the sidechain, -1 (cancelling) .. +1 (summing). Weighted towards the mids and highs (a +6 dB/oct tilt), where misalignment shows up as comb filtering; broadband correlation on a kick or bass would barely move. Averaged over ~300 ms. |

## ALIGN: how it measures

- It listens to this track's **raw input** (before SumAlign moves it) and the sidechain, on a background thread
  (the audio thread only copies samples into a lock-free buffer; it never waits or allocates).
- It finds the **reference's own hits** and takes one window per hit (starting 10 ms + the delay range before it).
- **Correlation, half-whitened and A-weighted:** the cross-spectrum is divided by the 4th root of both power spectra
  (so ringing drum tones don't swamp the timing), then weighted the way the ear hears loudness. The peak is refined to
  a fraction of a sample by exact band-limited interpolation; its sign is the polarity.
- **Sure** = the hits are dealt into 6 groups; it's how many groups, on their own, land on the same lag (within 2
  samples) and polarity. It finishes by itself after 36+ hits once 5 of 6 agree and the answer is stable; when playback
  stops with 4+ of 6 agreeing; or after 2 minutes. Below 4 of 6 it changes nothing.
- Other strong peaks become the **Earlier / Later** points (saved with the session).

**Chosen on real sessions** (Corey's drum session, the same kit and mics across songs, so a trustworthy measurement must
give the same answer every song). Plain, +6 dB/oct, A-weighted, PHAT, SCOT and coherence-weighted correlation were all
tried; only the half-whitened A-weighted one was consistent on every snare pair *and* physically sensible. Through the
plug-in's own listening/stopping logic, Songs 1 / 3 / 4, against the snare top:

| Track | Result (samples at 48 kHz) |
|---|---|
| Snare bottom | -21.8 / -21.1 / -22.2, inverted (0.45 ms) |
| Snare top LDC | +1.7 / +1.0 / +1.6 |
| Rack tom 1 | -37.2 / -36.8 / -36.5 |
| Hi-hat | -87.9 / -88.3 / -86.6 |
| OH L | -163.7 / -162.1 / -163.5 (3.4 ms) |
| Room L | -765.7 / -765.7 / -765.6, inverted (16 ms) |
| Kick port (vs kick in) | -52.1 / *not sure* / -52.7 |

It needed 20-50 s of each song (intros included). On synthetic pairs with HF loss, a room reflection, bleed and noise:
worst error 0.36 microseconds over 44.1-192 kHz across +-30 ms, both polarities.

## How it works

```
in -> fractional delay (64-tap Kaiser-windowed sinc) -> phase rotation (1st-order all-pass) -> polarity -> out
sidechain -> fixed latency -> correlation meter
```

- **Latency:** fixed at 30 ms + 31 samples (1471 samples at 48 kHz), the same on every instance and every setting, reported
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
- `tests/test_analysis.cpp`: ALIGN on synthetic mic pairs (fractional lags in both directions at 4 sample rates,
  polarity, kick in/out, unrelated and silent tracks = "not sure", alternate points, the real background thread,
  no allocations in the audio thread's capture).
- `tests/test_processor.cpp`:
  - bus layouts
  - reported latency and impulse timing in normal / bypassed / shifted processing at 44.1 - 192 kHz
  - the sidechain driving the meter
  - session save/load and typed values
  - ALIGN end to end through the real processor (Delay and Polarity set, points saved with the session)
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
- `Source/AlignAnalysis.h`: ALIGN (FFT, capture ring, pair estimator, background analyzer). Plain C++, no JUCE: it's
  the part that gets ported to The Gate later.
- `Source/Params.h`: parameter IDs (don't rename after release: it breaks sessions). Plug-in code `SmAl`, manufacturer `Nrka`: keep forever.
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`: JUCE wrapper and UI.
- `tools/Snapshot.cpp`: renders the real editor to a PNG (`-DSA_BUILD_TOOLS=ON`).
