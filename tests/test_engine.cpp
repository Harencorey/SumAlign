//==============================================================================
// SumAlign - offline engine tests (no JUCE, no DAW).
//
// Build & run:  cmake --build build --target AlignTests && ./build/AlignTests
//==============================================================================

#include "AlignCore.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

//------------------------------------------------------------------------------
// Allocation spy: counts heap allocations while the audio path runs.
static std::atomic<bool> gWatchAllocs { false };
static std::atomic<int>  gAllocCount { 0 };

void* operator new (std::size_t n)
{
    if (gWatchAllocs.load()) ++gAllocCount;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

static int gFailures = 0;
#define CHECK(cond, ...) \
    do { if (! (cond)) { ++gFailures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

using Buf = std::vector<std::vector<float>>;
static const double kRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

struct Change { size_t at; sa::Settings s; };

// Runs the engine over the whole buffer in blocks (blockSize 0 = random 1..1024).
// Settings changes take effect at the start of the block that contains their sample.
static Buf run (const Buf& in, sa::Settings s, double sr, int blockSize = 512,
                const Buf* ref = nullptr, std::vector<Change> changes = {},
                bool watchAllocs = false, sa::AlignEngine* keep = nullptr)
{
    sa::AlignEngine local;
    sa::AlignEngine& e = keep ? *keep : local;
    const int ch = (int) in.size();
    e.prepare (sr, ch);
    e.setSettings (s);
    e.reset();
    Buf out = in;
    std::mt19937 rng (7);
    std::uniform_int_distribution<int> dist (1, 1024);
    const size_t n = in[0].size();
    std::vector<float*> io ((size_t) ch);
    std::vector<const float*> rp (ref ? ref->size() : 0);
    size_t ci = 0;
    if (watchAllocs) { gAllocCount = 0; gWatchAllocs = true; }
    for (size_t pos = 0; pos < n;)
    {
        size_t len = (size_t) (blockSize > 0 ? blockSize : dist (rng));
        len = std::min (len, n - pos);
        while (ci < changes.size() && changes[ci].at <= pos) e.setSettings (changes[ci++].s);
        for (int c = 0; c < ch; ++c) io[(size_t) c] = out[(size_t) c].data() + pos;
        for (size_t c = 0; c < rp.size(); ++c) rp[c] = (*ref)[c].data() + pos;
        e.process (io.data(), ch, ref ? rp.data() : nullptr, (int) rp.size(), (int) len);
        pos += len;
    }
    gWatchAllocs = false;
    return out;
}

static std::vector<float> noise (size_t n, unsigned seed, float amp = 0.5f)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> d (0.0f, amp * 0.3f);
    std::vector<float> v (n);
    for (auto& x : v) x = d (rng);
    return v;
}

// Band-limited noise: white noise through a steep linear-phase low-pass (cutoff fraction of fs).
static std::vector<float> bandNoise (size_t n, unsigned seed, double cutoff)
{
    auto w = noise (n + 512, seed);
    const int L = 255;
    std::vector<double> h ((size_t) L);
    double sum = 0;
    for (int i = 0; i < L; ++i)
    {
        const double x = i - (L - 1) / 2.0;
        const double s = std::abs (x) < 1e-12 ? 2 * cutoff : std::sin (2 * sa::kPi * cutoff * x) / (sa::kPi * x);
        const double win = 0.42 - 0.5 * std::cos (2 * sa::kPi * i / (L - 1)) + 0.08 * std::cos (4 * sa::kPi * i / (L - 1));
        h[(size_t) i] = s * win; sum += h[(size_t) i];
    }
    std::vector<float> v (n);
    for (size_t i = 0; i < n; ++i)
    {
        double acc = 0;
        for (int k = 0; k < L; ++k) acc += h[(size_t) k] / sum * w[i + 512 - (size_t) k];
        v[i] = (float) acc;
    }
    return v;
}

static double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / std::max<size_t> (1, to - from));
}
static double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-30)); }

// Sine amplitude/phase at frequency f over [from, to) (least squares on sin/cos).
static void sineFit (const std::vector<float>& v, size_t from, size_t to, double f, double sr, double& amp, double& phase)
{
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (size_t i = from; i < to; ++i)
    {
        const double t = 2 * sa::kPi * f * (double) i / sr;
        const double s = std::sin (t), c = std::cos (t);
        ss += s * s; cc += c * c; sc += s * c; ys += v[i] * s; yc += v[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det;   // v ~ A sin + B cos
    amp = std::hypot (A, B);
    phase = std::atan2 (B, A);   // v ~ amp * sin(t + phase)
}

//==============================================================================
static void testLatencyAndZeroDelay()
{
    std::printf ("Latency, delay 0 = bit-exact delayed copy, bypass\n");
    for (double sr : kRates)
    {
        const size_t n = 20000;
        Buf in { noise (n, 1), noise (n, 2) };
        sa::AlignEngine e;
        auto out = run (in, {}, sr, 512, nullptr, {}, false, &e);
        const int L = e.latencySamples();
        CHECK (L == (int) std::lround (0.01 * sr) + sa::kInterpHalf, "latency %d at %.0f", L, sr);
        bool exact = true;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < n; ++i)
                if (out[(size_t) c][i] != (i >= (size_t) L ? in[(size_t) c][i - (size_t) L] : 0.0f)) { exact = false; break; }
        CHECK (exact, "delay 0 not a bit-exact copy at %.0f Hz", sr);

        // bypass gives exactly the same thing
        Buf b = in;
        sa::AlignEngine eb; eb.prepare (sr, 2); eb.reset();
        float* io[2] = { b[0].data(), b[1].data() };
        eb.processBypassed (io, 2, (int) n);
        CHECK (b == out, "bypass differs from delay 0 at %.0f Hz", sr);
    }
}

static void testIntegerDelays()
{
    std::printf ("Whole-sample delays are exact, both directions, full range\n");
    const double sr = 48000.0;
    const size_t n = 8000;
    Buf in { noise (n, 3) };
    sa::AlignEngine probe; probe.prepare (sr, 1);
    const int L = probe.latencySamples(), M = probe.maxShiftSamples();
    for (int d : { -M, -137, -1, 1, 17, 255, M, M + 50 /* clamps */ })
    {
        sa::Settings s; s.delaySamples = d;
        auto out = run (in, s, sr);
        const int dd = std::clamp (d, -M, M);
        bool exact = true;
        for (size_t i = (size_t) (L + M); i < n; ++i)
            if (out[0][i] != in[0][i - (size_t) (L + dd)]) { exact = false; break; }
        CHECK (exact, "delay %d samples not exact", d);
    }
}

static void testFractionalAccuracy()
{
    std::printf ("Fractional delays: sines land on the analytic delayed sine\n");
    for (double sr : kRates)
    {
        const size_t n = (size_t) (sr * 0.3);
        for (double frac : { 0.25, 0.5, 0.731 })
        {
            for (double f : { 60.0, 1000.0, 8000.0, 16000.0 })
            {
                std::vector<float> x (n);
                for (size_t i = 0; i < n; ++i) x[i] = (float) (0.5 * std::sin (2 * sa::kPi * f * (double) i / sr));
                sa::Settings s; s.delaySamples = -3.0 + frac;
                sa::AlignEngine e;
                auto out = run ({ x }, s, sr, 512, nullptr, {}, false, &e);
                const double D = e.latencySamples() + s.delaySamples;
                double err = 0;
                const size_t from = n / 3;
                for (size_t i = from; i < n; ++i)
                {
                    const double ideal = 0.5 * std::sin (2 * sa::kPi * f * ((double) i - D) / sr);
                    err += (out[0][i] - ideal) * (out[0][i] - ideal);
                }
                const double errDb = db (std::sqrt (err / (double) (n - from)) / (0.5 / std::sqrt (2.0)));
                CHECK (errDb < -95.0, "%.0f Hz at %.0f, frac %.3f: error %.1f dB", f, sr, frac, errDb);
            }
        }
    }
}

static void testNullAndRecovery()
{
    std::printf ("Null test: a delayed, polarity-flipped copy aligned back cancels\n");
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const size_t n = (size_t) (sr * 0.5);
        auto ref = bandNoise (n + 2000, 11, 0.4);   // band-limited, like any real recording
        for (double lag : { 3.37, 41.5, 0.12, 211.808 })
        {
            // "mic 2" hears the source lag samples later and in reverse polarity
            std::vector<float> late (n), src (n);
            const int whole = (int) std::floor (lag);
            const double fr = lag - whole;
            sa::Taps t; t.design (whole + fr + sa::kInterpHalf);   // exact reference interpolator
            for (size_t i = 0; i < n; ++i)
            {
                src[i] = ref[i + 1000];
                double acc = 0;
                for (int k = 0; k < sa::kTaps; ++k)
                {
                    const long idx = (long) i + 1000 - t.whole - (k - sa::kInterpHalf) + sa::kInterpHalf;
                    acc += t.h[(size_t) k] * ref[(size_t) idx];
                }
                late[i] = (float) -acc;
            }
            sa::Settings s; s.delaySamples = -lag; s.invert = true;
            sa::AlignEngine e;
            auto out = run ({ late }, s, sr, 256, nullptr, {}, false, &e);
            sa::AlignEngine z;
            auto dry = run ({ src }, {}, sr, 256, nullptr, {}, false, &z);   // reference track's instance at 0
            std::vector<float> diff (n);
            for (size_t i = 0; i < n; ++i) diff[i] = out[0][i] - dry[0][i];
            const size_t from = n / 4;
            const double depth = db (rms (diff, from, n) / rms (dry[0], from, n));
            CHECK (depth < -85.0, "lag %.3f at %.0f: null only %.1f dB", lag, sr, depth);
        }
    }
}

static void testPolarity()
{
    std::printf ("Polarity: exact inversion, click-free switching\n");
    const double sr = 48000.0;
    const size_t n = 48000;
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i) x[i] = (float) (0.5 * std::sin (2 * sa::kPi * 100.0 * (double) i / sr));
    sa::Settings inv; inv.invert = true;
    auto a = run ({ x }, inv, sr);
    sa::AlignEngine e; auto b = run ({ x }, {}, sr, 512, nullptr, {}, false, &e);
    bool exact = true;
    for (size_t i = 0; i < n; ++i) if (a[0][i] != -b[0][i]) { exact = false; break; }
    CHECK (exact, "inverted output is not exactly -1 x the normal output");

    // flip mid-stream: the largest sample-to-sample step stays near the sine's own
    auto c = run ({ x }, {}, sr, 64, nullptr, { { 24000, inv } });
    double maxStep = 0, sineStep = 2 * sa::kPi * 100.0 / sr * 0.5;
    for (size_t i = 1; i < n; ++i) maxStep = std::max (maxStep, (double) std::abs (c[0][i] - c[0][i - 1]));
    CHECK (maxStep < 4.0 * sineStep, "polarity switch steps %.5f (sine %.5f): click", maxStep, sineStep);
}

static void testPhaseRotation()
{
    std::printf ("Phase knob: all-pass (flat), exact angle at Phase Center, 0 = bit-identical\n");
    for (double sr : kRates)
    {
        const size_t n = (size_t) (sr * 0.5);
        for (double fc : { 80.0, 250.0, 1200.0, 5000.0 })
            for (double deg : { 30.0, 90.0, 150.0, 179.0 })
            {
                sa::Settings s; s.phaseDeg = deg; s.phaseCenterHz = fc;
                // measured angle at the centre
                std::vector<float> x (n);
                for (size_t i = 0; i < n; ++i) x[i] = (float) (0.5 * std::sin (2 * sa::kPi * fc * (double) i / sr));
                auto y = run ({ x }, s, sr);
                auto z = run ({ x }, {}, sr);
                double ay, py, az, pz;
                sineFit (y[0], n / 2, n, fc, sr, ay, py);
                sineFit (z[0], n / 2, n, fc, sr, az, pz);
                double lag = (pz - py) * 180.0 / sa::kPi;
                while (lag < -1e-9) lag += 360.0;
                while (lag >= 360.0) lag -= 360.0;
                CHECK (std::abs (lag - deg) < 0.05, "%.0f deg at %.0f Hz (%.0f): measured %.2f", deg, fc, sr, lag);
                CHECK (std::abs (db (ay / az)) < 0.01, "gain %.4f dB at the centre", db (ay / az));
                const double ui = sa::AlignEngine::phaseShiftDegAt (s, fc, sr);
                CHECK (std::abs (ui - deg) < 0.01, "UI curve says %.3f deg, want %.0f", ui, deg);
            }
        // flat magnitude everywhere
        sa::Settings s; s.phaseDeg = 120.0; s.phaseCenterHz = 300.0;
        for (double f : { 30.0, 300.0, 3000.0, 15000.0 })
        {
            std::vector<float> x (n);
            for (size_t i = 0; i < n; ++i) x[i] = (float) (0.5 * std::sin (2 * sa::kPi * f * (double) i / sr));
            auto y = run ({ x }, s, sr);
            double a, p; sineFit (y[0], n / 2, n, f, sr, a, p);
            CHECK (std::abs (db (a / 0.5)) < 0.01, "magnitude %.4f dB at %.0f Hz", db (a / 0.5), f);
        }
    }
    // 0 deg is bit-identical to the plain delay, also after having been on
    const double sr = 48000.0;
    Buf in { noise (48000, 5), noise (48000, 6) };
    sa::Settings on; on.phaseDeg = 90.0;
    auto plain = run (in, {}, sr);
    auto back = run (in, {}, sr, 512, nullptr, { { 0, on }, { 9600, {} } });
    bool same = true;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 24000; i < 48000; ++i) if (back[(size_t) c][i] != plain[(size_t) c][i]) { same = false; break; }
    CHECK (same, "phase back to 0 is not bit-identical to off");
}

static void testClickFree()
{
    std::printf ("Delay and phase moves are click-free\n");
    const double sr = 48000.0;
    const size_t n = 96000;
    std::vector<float> x (n);
    const double f = 220.0;
    for (size_t i = 0; i < n; ++i) x[i] = (float) (0.5 * std::sin (2 * sa::kPi * f * (double) i / sr));
    const double sineStep = 2 * sa::kPi * f / sr * 0.5;
    sa::Settings a; a.delaySamples = 120.3;
    sa::Settings b; b.delaySamples = -77.6;
    sa::Settings c = b; c.phaseDeg = 160.0;
    sa::Settings d = c; d.phaseCenterHz = 2000.0;
    sa::Settings e = d; e.phaseDeg = 0.0;
    auto y = run ({ x }, {}, sr, 128, nullptr, { { 10000, a }, { 20000, b }, { 30000, c }, { 40000, d }, { 50000, e } });
    double maxStep = 0;
    for (size_t i = 1; i < n; ++i) maxStep = std::max (maxStep, (double) std::abs (y[0][i] - y[0][i - 1]));
    CHECK (maxStep < 3.0 * sineStep, "largest step %.5f vs the sine's %.5f: click", maxStep, sineStep);
    std::printf ("  largest step %.2f x the sine's own\n", maxStep / sineStep);
}

static void testBlockSizeIndependence()
{
    std::printf ("Output is identical at any block size (incl. changes)\n");
    const double sr = 44100.0;
    const size_t n = 32768;
    Buf in { noise (n, 9), noise (n, 10) };
    Buf ref { noise (n, 12) };
    sa::Settings a; a.delaySamples = 33.3; a.phaseDeg = 45; a.phaseCenterHz = 600;
    sa::Settings b = a; b.delaySamples = -12.25; b.invert = true;
    std::vector<Change> ch { { 8192, a }, { 16384, b } };
    auto base = run (in, {}, sr, 1, &ref, ch);
    for (int bs : { 32, 64, 512, 1024, 4096 })
    {
        auto o = run (in, {}, sr, bs, &ref, ch);
        CHECK (o == base, "block size %d differs from block size 1", bs);
    }
}

static void testCorrelation()
{
    std::printf ("Correlation meter: +1 aligned, -1 flipped, ~0 unrelated, rises when aligned\n");
    const double sr = 48000.0;
    const size_t n = 96000;
    auto s = bandNoise (n, 21, 0.2);
    auto u = bandNoise (n, 22, 0.2);
    auto meter = [&] (const std::vector<float>& x, const std::vector<float>& r, sa::Settings st)
    {
        sa::AlignEngine e;
        Buf rb { r };
        run ({ x }, st, sr, 512, &rb, {}, false, &e);
        return e.correlation.load();
    };
    float same = meter (s, s, {});
    std::vector<float> neg (n); for (size_t i = 0; i < n; ++i) neg[i] = -s[i];
    float flip = meter (neg, s, {});
    float none = meter (u, s, {});
    CHECK (same > 0.99f, "same signal reads %.3f", same);
    CHECK (flip < -0.99f, "inverted reads %.3f", flip);
    CHECK (std::abs (none) < 0.1f, "unrelated reads %.3f", none);

    // a track 1.3 ms late reads low; with delay -1.3 ms it reads ~+1
    const int lag = 62;
    std::vector<float> late (n, 0.0f);
    for (size_t i = (size_t) lag; i < n; ++i) late[i] = s[i - (size_t) lag];
    float before = meter (late, s, {});
    sa::Settings fix; fix.delaySamples = -lag;
    float after = meter (late, s, fix);
    CHECK (after > 0.99f && after > before + 0.3f, "late %.3f -> aligned %.3f", before, after);

    // a bass-heavy source (kick-like) 1.3 ms late must still read clearly lower than aligned
    std::vector<float> kick (n, 0.0f);
    std::mt19937 rng (4);
    std::uniform_real_distribution<float> uu (-1.0f, 1.0f);
    for (int hit = 0; hit < 8; ++hit)
        for (int i = 0; i < 9000; ++i)
        {
            const size_t at = 2000 + (size_t) hit * 11000 + (size_t) i;
            if (at >= n) break;
            const double t = i / sr, fr = 50.0 + 90.0 * std::exp (-t / 0.02);
            kick[at] += (float) (0.6 * std::exp (-t / 0.09) * std::sin (2 * sa::kPi * fr * t) + (i < 200 ? 0.3 * uu (rng) * std::exp (-t / 0.001) : 0.0));
        }
    std::vector<float> kickLate (n, 0.0f);
    for (size_t i = (size_t) lag; i < n; ++i) kickLate[i] = kick[i - (size_t) lag];
    const float kb = meter (kickLate, kick, {});
    const float ka = meter (kickLate, kick, fix);
    std::printf ("  kick-like: 1.3 ms late %.2f, aligned %.2f\n", kb, ka);
    CHECK (ka > 0.98f && kb < 0.8f, "kick-like late %.3f / aligned %.3f: meter not sensitive enough", kb, ka);

    // no reference = nothing to show
    sa::AlignEngine e;
    run ({ s }, {}, sr, 512, nullptr, {}, false, &e);
    CHECK (! e.refPresent.load() && e.correlation.load() == 0.0f, "no reference but meter shows %.3f", e.correlation.load());
}

static void testNoAllocations()
{
    std::printf ("No heap allocations on the audio path\n");
    const double sr = 96000.0;
    const size_t n = 50000;
    Buf in { noise (n, 13), noise (n, 14) };
    Buf ref { noise (n, 15) };
    sa::Settings a; a.delaySamples = 300.5; a.phaseDeg = 90; a.invert = true;
    sa::Settings b; b.delaySamples = -400.25;
    sa::AlignEngine e;
    e.prepare (sr, 2); e.reset();
    // prepare happens before watching; changes during processing must not allocate
    Buf out = in;
    float* io[2] = { out[0].data(), out[1].data() };
    const float* rp[1] = { ref[0].data() };
    gAllocCount = 0; gWatchAllocs = true;
    for (size_t pos = 0; pos + 500 <= n; pos += 500)
    {
        e.setSettings (pos < n / 3 ? sa::Settings {} : pos < 2 * n / 3 ? a : b);
        float* p[2] = { io[0] + pos, io[1] + pos };
        const float* r[1] = { rp[0] + pos };
        e.process (p, 2, r, 1, 500);
        e.processBypassed (p, 2, 0);
    }
    gWatchAllocs = false;
    CHECK (gAllocCount == 0, "%d allocations", gAllocCount.load());
}

static void testMonoAndSilence()
{
    std::printf ("Mono works; silence in = silence out; no denormals or NaN\n");
    const double sr = 44100.0;
    Buf in { std::vector<float> (44100, 0.0f) };
    sa::Settings s; s.delaySamples = 10.5; s.phaseDeg = 179.0; s.phaseCenterHz = 20.0; s.invert = true;
    auto out = run (in, s, sr);
    bool silent = true;
    for (float v : out[0]) if (v != 0.0f || std::isnan (v)) { silent = false; break; }
    CHECK (silent, "silence in did not give exact silence out");

    // a full-scale impulse through the most extreme setting decays cleanly
    in[0][100] = 1.0f;
    out = run (in, s, sr);
    bool finite = true; double tail = 0;
    for (size_t i = 0; i < out[0].size(); ++i) { if (! std::isfinite (out[0][i])) finite = false; if (i > 40000) tail = std::max (tail, (double) std::abs (out[0][i])); }
    CHECK (finite, "non-finite output");
    CHECK (tail < 1e-4, "impulse tail still %.2e after 0.9 s", tail);
}

int main()
{
    testLatencyAndZeroDelay();
    testIntegerDelays();
    testFractionalAccuracy();
    testNullAndRecovery();
    testPolarity();
    testPhaseRotation();
    testClickFree();
    testBlockSizeIndependence();
    testCorrelation();
    testNoAllocations();
    testMonoAndSilence();

    if (gFailures == 0) std::printf ("\nALL TESTS PASSED\n");
    else                std::printf ("\n%d FAILURE(S)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
