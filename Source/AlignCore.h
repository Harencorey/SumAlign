#pragma once
//==============================================================================
// SumAlign - the whole audio path, plain C++ (no JUCE), shared by the plug-in and the tests.
//
//   in -> fractional delay (windowed sinc) -> phase rotation (1st-order all-pass) -> polarity -> out
//
// Latency: every instance reports the same fixed latency, kMaxShiftMs + the interpolator's half
// length. Delay 0 = the input delayed by exactly that latency (bit-exact, no filtering). Negative
// delay values move the track *earlier* than that, down to -kMaxShiftMs.
//
// Real-time safe: all memory is allocated in prepare(); process() never allocates, locks or logs.
//==============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sa
{

constexpr double kPi          = 3.14159265358979323846;
constexpr double kMaxShiftMs  = 30.0;   // delay range: -30 ms ... +30 ms (room mics up to ~10 m)
constexpr int    kTaps        = 64;     // windowed-sinc length (fractional part only)
constexpr int    kHalf        = kTaps / 2;
constexpr int    kInterpHalf  = kHalf - 1;   // samples of "look-ahead" the interpolator needs
constexpr double kKaiserBeta  = 9.0;    // ~ -90 dB stop band
constexpr double kDelayFadeMs = 10.0;   // crossfade when the delay changes
constexpr double kFlipFadeMs  = 10.0;   // polarity ramps through zero instead of clicking
constexpr double kPhaseFadeMs = 20.0;   // phase rotation fades in/out; coefficient glide time
constexpr double kCorrTauMs   = 300.0;  // correlation meter averaging
constexpr int    kMaxChannels = 2;

struct Settings
{
    double delaySamples  = 0.0;     // + later, - earlier; clamped to +-maxShiftSamples()
    bool   invert        = false;
    double phaseDeg      = 0.0;     // 0..180: phase shift at phaseCenterHz
    double phaseCenterHz = 250.0;
};

//------------------------------------------------------------------------------
// Modified Bessel function I0 (series), for the Kaiser window.
inline double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;
        if (term < sum * 1.0e-17) break;
    }
    return sum;
}

//------------------------------------------------------------------------------
// One set of interpolation taps for one delay value.
struct Taps
{
    int    whole = 0;          // integer part of the total delay
    bool   integer = true;     // fractional part is 0: a plain sample copy (bit-exact)
    double delay = 0.0;        // the total delay these taps implement
    std::array<float, kTaps> h {};

    // Total delay D >= kInterpHalf. y[n] = sum_k h[k] * x[n - whole - (k - kInterpHalf)].
    void design (double D) noexcept
    {
        delay = D;
        whole = (int) std::floor (D);
        double f = D - whole;
        if (f < 1.0e-9) f = 0.0;
        if (f > 1.0 - 1.0e-9) { f = 0.0; ++whole; }
        integer = (f == 0.0);
        if (integer) { h.fill (0.0f); h[(size_t) kInterpHalf] = 1.0f; return; }

        static const double i0b = besselI0 (kKaiserBeta);
        double sum = 0.0;
        std::array<double, kTaps> t {};
        for (int i = 0; i < kTaps; ++i)
        {
            const double x = (double) (i - kInterpHalf) - f;           // distance from the true sample point
            const double s = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double r = x / (double) kHalf;                        // -1..1 across the window
            const double w = r * r < 1.0 ? besselI0 (kKaiserBeta * std::sqrt (1.0 - r * r)) / i0b : 0.0;
            t[(size_t) i] = s * w;
            sum += t[(size_t) i];
        }
        for (int i = 0; i < kTaps; ++i)
            h[(size_t) i] = (float) (t[(size_t) i] / sum);   // unity gain at DC
    }
};

//------------------------------------------------------------------------------
class AlignEngine
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        maxShift = (int) std::lround (kMaxShiftMs * 0.001 * sr);
        latency  = maxShift + kInterpHalf;

        int need = 2 * maxShift + kTaps + 8;
        size = 1;
        while (size < need) size <<= 1;
        mask = size - 1;
        for (auto& b : ring) b.assign ((size_t) size, 0.0f);
        refRing.assign ((size_t) size, 0.0f);

        delayFadeLen = std::max (1, (int) std::lround (kDelayFadeMs * 0.001 * sr));
        flipStep     = 2.0 / std::max (1.0, kFlipFadeMs * 0.001 * sr);
        phaseFadeLen = std::max (1, (int) std::lround (kPhaseFadeMs * 0.001 * sr));
        corrCoeff    = 1.0 - std::exp (-1.0 / (kCorrTauMs * 0.001 * sr));
        reset();
    }

    void reset() noexcept
    {
        for (auto& b : ring) std::fill (b.begin(), b.end(), 0.0f);
        std::fill (refRing.begin(), refRing.end(), 0.0f);
        writePos = 0;
        cur.design (latency + clampDelay (settings.delaySamples));
        next = cur;
        fadePos = -1;
        flipGain = settings.invert ? -1.0 : 1.0;
        phaseA = phaseTargetA = coeffFor (settings);
        phaseWet = targetPhaseActive (settings) ? 1.0 : 0.0;
        phaseActive = phaseWet > 0.0;
        apState.fill (0.0);
        sxy = sxx = syy = 0.0;
        refPrev = outPrev = 0.0;
        correlation.store (0.0f);
        refPresent.store (false);
    }

    void setSettings (const Settings& s) noexcept { settings = s; }

    int    latencySamples()  const noexcept { return latency; }
    int    maxShiftSamples() const noexcept { return maxShift; }
    double sampleRate()      const noexcept { return sr; }

    double clampDelay (double d) const noexcept { return std::clamp (d, (double) -maxShift, (double) maxShift); }

    // io: numCh channel pointers, processed in place. ref: optional reference (the sidechain), refCh channels.
    void process (float* const* io, int numCh, const float* const* ref, int refCh, int n) noexcept
    {
        numCh = std::min (numCh, channels);
        const double wantDelay = latency + clampDelay (settings.delaySamples);
        const double wantFlip  = settings.invert ? -1.0 : 1.0;
        const bool   wantPhase = targetPhaseActive (settings);
        const double wantA     = coeffFor (settings);
        const double aStep     = 1.0 / (double) phaseFadeLen;

        // Phase rotation: switching on starts at the new coefficient (wet fades in from 0);
        // while on, the coefficient glides; switching off fades the wet path out, then stops it.
        if (wantPhase && ! phaseActive)
        {
            phaseActive = true;
            phaseA = phaseTargetA = wantA;
            apState.fill (0.0);
        }
        if (wantPhase) phaseTargetA = wantA;

        bool anyRef = false;

        for (int i = 0; i < n; ++i)
        {
            // --- delay target: start a crossfade when it changes (one at a time) ---------------
            if (fadePos < 0 && wantDelay != cur.delay)
            {
                next.design (wantDelay);
                fadePos = 0;
            }

            const int w = writePos;
            for (int c = 0; c < numCh; ++c)
                ring[(size_t) c][(size_t) w] = io[c][i];

            float mono = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                double y = read (c, cur);
                if (fadePos >= 0)
                {
                    const double t = (double) (fadePos + 1) / (double) delayFadeLen;
                    y += t * (read (c, next) - y);
                }

                // --- phase rotation (1st-order all-pass, transposed direct form II) -------------
                if (phaseActive)
                {
                    const double ap = phaseA * y + apState[(size_t) c];
                    apState[(size_t) c] = y - phaseA * ap;
                    y += phaseWet * (ap - y);
                }

                y *= flipGain;
                io[c][i] = (float) y;
                mono += (float) y;
            }

            // --- advance the smoothers (once per sample, shared by all channels) ------------------
            if (fadePos >= 0 && ++fadePos >= delayFadeLen) { cur = next; fadePos = -1; }

            if (flipGain != wantFlip)
                flipGain = wantFlip > flipGain ? std::min (wantFlip, flipGain + flipStep)
                                               : std::max (wantFlip, flipGain - flipStep);

            if (phaseActive)
            {
                if (wantPhase)
                {
                    phaseWet = std::min (1.0, phaseWet + aStep);
                    const double gl = 1.0 / (double) phaseFadeLen;   // linear glide of the coefficient
                    const double d = phaseTargetA - phaseA;
                    phaseA = std::abs (d) <= 2.0 * gl ? phaseTargetA : phaseA + (d > 0 ? 2.0 * gl : -2.0 * gl);
                }
                else
                {
                    phaseWet = std::max (0.0, phaseWet - aStep);
                    if (phaseWet <= 0.0) { phaseActive = false; apState.fill (0.0); }
                }
            }

            // --- correlation vs the reference, delayed by the same fixed latency -----------------
            if (ref != nullptr && refCh > 0)
            {
                float r = 0.0f;
                for (int c = 0; c < refCh; ++c) r += ref[c][i];
                refRing[(size_t) w] = r;
                // first difference of both (+6 dB/oct tilt): misalignment shows up as comb
                // filtering in the mids and highs, which plain broadband correlation (dominated
                // by the lows on drums and bass) would barely register
                const double rd = refRing[(size_t) ((w - latency) & mask)];
                const double rr = rd - refPrev, yy = (double) mono - outPrev;
                refPrev = rd; outPrev = mono;
                sxy += corrCoeff * (yy * rr - sxy);
                sxx += corrCoeff * (yy * yy - sxx);
                syy += corrCoeff * (rr * rr - syy);
                anyRef = true;
            }

            writePos = (w + 1) & mask;
        }

        // flush denormals in the slow accumulators
        if (std::abs (sxy) < 1.0e-30) sxy = 0.0;
        if (std::abs (sxx) < 1.0e-30) sxx = 0.0;
        if (std::abs (syy) < 1.0e-30) syy = 0.0;
        for (auto& s : apState) if (std::abs (s) < 1.0e-30) s = 0.0;

        const bool live = anyRef && syy > 1.0e-10 && sxx > 1.0e-10;   // ~ -100 dBFS
        refPresent.store (anyRef);
        correlation.store (live ? (float) std::clamp (sxy / std::sqrt (sxx * syy), -1.0, 1.0) : 0.0f);
        signalPresent.store (live);
    }

    // Host bypass: the input delayed by exactly the reported latency, so bypassing never shifts
    // the track. Keeps the delay line filled so switching back is seamless.
    void processBypassed (float* const* io, int numCh, int n) noexcept
    {
        numCh = std::min (numCh, channels);
        for (int i = 0; i < n; ++i)
        {
            const int w = writePos;
            for (int c = 0; c < numCh; ++c)
            {
                ring[(size_t) c][(size_t) w] = io[c][i];
                io[c][i] = ring[(size_t) c][(size_t) ((w - latency) & mask)];
            }
            refRing[(size_t) w] = 0.0f;
            writePos = (w + 1) & mask;
        }
    }

    // For the UI: phase shift (degrees, 0 = none, positive = lagging) at frequency hz for settings s.
    static double phaseShiftDegAt (const Settings& s, double hz, double sampleRate) noexcept
    {
        if (! targetPhaseActive (s)) return 0.0;
        const double a = coeffFor (s, sampleRate);
        const double wv = 2.0 * kPi * hz / sampleRate;
        // H = (a + e^-jw) / (1 + a e^-jw)
        const double nr = a + std::cos (wv), ni = -std::sin (wv);
        const double dr = 1.0 + a * std::cos (wv), di = -a * std::sin (wv);
        double ph = std::atan2 (ni, nr) - std::atan2 (di, dr);
        while (ph > 0.0) ph -= 2.0 * kPi;
        while (ph <= -2.0 * kPi) ph += 2.0 * kPi;
        return -ph * 180.0 / kPi;
    }

    std::atomic<float> correlation { 0.0f };   // -1..+1, output vs reference
    std::atomic<bool>  refPresent { false };   // a reference (sidechain) is connected
    std::atomic<bool>  signalPresent { false };// both have signal: the correlation is meaningful

private:
    static bool targetPhaseActive (const Settings& s) noexcept { return s.phaseDeg > 1.0e-6; }

    double coeffFor (const Settings& s) const noexcept { return coeffFor (s, sr); }

    // Bilinear 1st-order all-pass H(z) = (a + z^-1)/(1 + a z^-1): phase 0 at DC, -180 deg at Nyquist,
    // exactly -phaseDeg at phaseCenterHz.
    static double coeffFor (const Settings& s, double rate) noexcept
    {
        constexpr double kAMax = 0.9995;     // "off" end: keeps the pole off the unit circle
        constexpr double kAMin = -0.99999;   // 180 deg end: allows ~179.9 deg down to 20 Hz centres
        if (! targetPhaseActive (s)) return kAMax;
        const double fc = std::clamp (s.phaseCenterHz, 10.0, 0.45 * rate);
        const double w0 = std::tan (kPi * fc / rate);
        const double th = std::clamp (s.phaseDeg, 0.0, 180.0) * kPi / 180.0;
        const double t  = std::tan (0.5 * th);
        if (t > 1.0e12) return kAMin;
        const double wc = w0 / t;
        return std::clamp ((wc - 1.0) / (wc + 1.0), kAMin, kAMax);
    }

    inline double read (int c, const Taps& t) const noexcept
    {
        const auto& b = ring[(size_t) c];
        if (t.integer)
            return b[(size_t) ((writePos - t.whole) & mask)];
        // tap i reads x[n - whole - (i - kInterpHalf)]
        const int base = writePos - t.whole + kInterpHalf;
        double acc = 0.0;
        for (int i = 0; i < kTaps; ++i)
            acc += (double) t.h[(size_t) i] * (double) b[(size_t) ((base - i) & mask)];
        return acc;
    }

    double sr = 48000.0;
    int channels = 2, maxShift = 480, latency = 511;
    int size = 0, mask = 0, writePos = 0;
    std::array<std::vector<float>, kMaxChannels> ring;
    std::vector<float> refRing;

    Settings settings;
    Taps cur, next;
    int fadePos = -1, delayFadeLen = 480;

    double flipGain = 1.0, flipStep = 0.004;

    bool phaseActive = false;
    double phaseA = 0.9995, phaseTargetA = 0.9995, phaseWet = 0.0;
    int phaseFadeLen = 960;
    std::array<double, kMaxChannels> apState {};

    double sxy = 0.0, sxx = 0.0, syy = 0.0, corrCoeff = 0.0, refPrev = 0.0, outPrev = 0.0;
};

} // namespace sa
