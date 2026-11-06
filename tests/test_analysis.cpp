//==============================================================================
// SumAlign - ALIGN analysis tests on synthetic mic pairs (no JUCE, no DAW).
//
// Each "session" is a drum-like source heard by two mics:
//   reference = the source (close mic) + bleed from another instrument + noise
//   this track = polarity x (source, delayed by an exact fractional lag, darker with distance,
//                plus a room reflection) + different bleed + noise
// The fractional delay is applied exactly in the frequency domain, independent of SumAlign's own
// interpolator, so the true lag is known to well under 0.01 samples.
//==============================================================================

#include "AlignAnalysis.h"
#include "AlignCore.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>
#include <vector>

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

using Vec = std::vector<float>;
constexpr double kPi = 3.14159265358979323846;

// exact band-limited delay by tau samples (positive = later), via one big FFT (circular, padded)
static Vec delayExact (const Vec& x, double tau)
{
    int n = 1; while (n < (int) x.size() + 8192) n <<= 1;
    sa::Fft f; f.prepare (n);
    std::vector<sa::cplx> a ((size_t) n);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    f.run (a, false);
    for (int k = 0; k < n; ++k)
    {
        const int kk = k <= n / 2 ? k : k - n;
        double ph = -2.0 * kPi * kk * tau / n;
        if (k == n / 2) ph = 0.0;   // Nyquist bin: keep real
        a[(size_t) k] *= std::polar (1.0, ph);
    }
    f.run (a, true);
    Vec y (x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = (float) (a[i].real() / n);
    return y;
}

// zero-phase (symmetric FIR) low-pass: darker but no timing change
static Vec darker (const Vec& x, double cutoffHz, double sr)
{
    const int L = 63;
    std::vector<double> h (L);
    double s = 0;
    const double fc = cutoffHz / sr;
    for (int i = 0; i < L; ++i)
    {
        const double t = i - (L - 1) / 2.0;
        const double v = std::abs (t) < 1e-12 ? 2 * fc : std::sin (2 * kPi * fc * t) / (kPi * t);
        h[(size_t) i] = v * (0.54 - 0.46 * std::cos (2 * kPi * i / (L - 1)));
        s += h[(size_t) i];
    }
    Vec y (x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i)
    {
        double acc = 0;
        for (int k = 0; k < L; ++k)
        {
            const long j = (long) i + (L - 1) / 2 - k;
            if (j >= 0 && j < (long) x.size()) acc += h[(size_t) k] / s * x[(size_t) j];
        }
        y[i] = (float) acc;
    }
    return y;
}

// drum-like source: hits (pitched decaying body + noise click) at irregular times
static Vec drums (size_t n, double sr, unsigned seed, double bodyHz = 180.0, double rate = 3.0)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> u (-1.0, 1.0), gap (0.6, 1.4), amp (0.4, 1.0);
    Vec y (n, 0.0f);
    double t = 0.05 * sr;
    while (t < (double) n)
    {
        const size_t at = (size_t) t;
        const double a = amp (rng);
        const double f0 = bodyHz * (0.9 + 0.2 * std::abs (u (rng)));
        for (size_t i = 0; i < (size_t) (0.4 * sr) && at + i < n; ++i)
        {
            const double tt = (double) i / sr;
            const double body = std::exp (-tt / 0.12) * std::sin (2 * kPi * f0 * (1.0 + 0.5 * std::exp (-tt / 0.01)) * tt);
            const double click = std::exp (-tt / 0.004) * u (rng);
            y[at + i] += (float) (a * (0.6 * body + 0.5 * click));
        }
        t += sr / rate * gap (rng);
    }
    return y;
}

static Vec noise (size_t n, unsigned seed, double amp)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> d (0.0, amp);
    Vec v (n);
    for (auto& x : v) x = (float) d (rng);
    return v;
}

static void addTo (Vec& a, const Vec& b, double g) { for (size_t i = 0; i < a.size(); ++i) a[i] += (float) (g * b[i]); }

struct Pair { Vec x, r; };

static Pair session (double sr, double lag, int polarity, double seconds, unsigned seed,
                     bool reflection = true, double cutoffHz = 7000.0, double bleedDb = -18.0)
{
    const size_t n = (size_t) (seconds * sr);
    Vec src = drums (n, sr, seed);
    Vec other = drums (n, sr, seed + 100, 90.0, 2.1);   // e.g. the kick bleeding into a snare/OH pair
    Pair p;
    p.r = src;
    addTo (p.r, delayExact (other, 37.0), std::pow (10.0, bleedDb / 20.0));
    addTo (p.r, noise (n, seed + 1, 0.0005), 1.0);

    Vec direct = darker (delayExact (src, lag), cutoffHz, sr);
    p.x.assign (n, 0.0f);
    addTo (p.x, direct, 0.7 * polarity);
    if (reflection) addTo (p.x, darker (delayExact (src, lag + 0.004 * sr), 3000.0, sr), 0.25 * polarity);
    addTo (p.x, delayExact (other, -150.0), 0.3);   // other source arrives at this mic at a different time
    addTo (p.x, noise (n, seed + 2, 0.0005), 1.0);
    return p;
}

static sa::AlignAnalyzer::Status runAnalyzer (const Pair& p, double sr, int maxLag, bool finish = true)
{
    sa::AlignAnalyzer a;
    a.setThreaded (false);
    a.prepare (sr, maxLag);
    a.start();
    const int block = 512;
    for (size_t pos = 0; pos + block <= p.x.size(); pos += block)
    {
        a.push (p.x.data() + pos, p.r.data() + pos, block);
        if ((pos / block) % 16 == 0) a.pump();
        if (! a.isListening()) break;
    }
    a.pump();
    if (finish && a.getStatus().state == sa::AlignAnalyzer::State::listening) { a.finishNow(); a.pump(); }
    return a.getStatus();
}

//==============================================================================
static void testAccuracy()
{
    std::printf ("Finds fractional lags (both directions, full range) and polarity, through HF loss, a room reflection, bleed and noise\n");
    double worstUs = 0.0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int maxLag = (int) std::lround (sa::kMaxShiftMs * 0.001 * sr);
        const double k = sr / 48000.0;
        for (double lag48 : { 3.37, -12.8, 70.25, 700.6, -1100.5, 1400.9 })
        {
            const double lag = lag48 * k + 0.13;   // arbitrary fraction at every rate
            for (int pol : { 1, -1 })
            {
                auto p = session (sr, lag, pol, 10.0, (unsigned) (lag48 * 10 + pol + 1000));
                auto st = runAnalyzer (p, sr, maxLag);
                const auto& r = st.result;
                const double err = r.lagSamples - lag;
                CHECK (st.state == sa::AlignAnalyzer::State::done, "sr %.0f lag %.2f pol %d: not done (conf %.2f, agree %.2f, %d hits)",
                       sr, lag, pol, r.confidence, r.agreement, r.windows);
                // within 1.5 us (~0.5 mm of mic distance; 5 deg at 10 kHz) or 0.05 samples, whichever is larger
                const double tol = std::max (0.05, 1.5e-6 * sr);
                CHECK (std::abs (err) < tol, "sr %.0f lag %.2f pol %d: measured %.3f (err %.3f samples, %.2f us)", sr, lag, pol, r.lagSamples, err, err / sr * 1e6);
                worstUs = std::max (worstUs, std::abs (err) / sr * 1e6);
                CHECK (r.polarity == pol, "sr %.0f lag %.2f: polarity %d, want %d", sr, lag, r.polarity, pol);
                if (sr == 48000.0 && pol == 1)
                    std::printf ("  48k lag %8.2f -> %8.3f  (err %+.3f)  conf %.2f  agree %.2f  peak/rest %.1f  hits %d  heard %.1f s\n",
                                 lag, r.lagSamples, err, r.confidence, r.agreement, r.peakRatio, r.windows, st.heardSeconds);
            }
        }
    }
    std::printf ("  worst error over all rates, lags and polarities: %.2f us\n", worstUs);
}

static void testKickPair()
{
    std::printf ("Kick in / kick out: low-heavy, inverted, 25 samples apart\n");
    const double sr = 48000.0;
    const size_t n = (size_t) (12 * sr);
    Vec kick = drums (n, sr, 77, 55.0, 2.0);
    Pair p;
    p.r = kick; addTo (p.r, noise (n, 3, 0.0003), 1.0);
    p.x = darker (delayExact (kick, 25.4), 4000.0, sr);
    for (auto& v : p.x) v = -v;
    addTo (p.x, noise (n, 4, 0.0003), 1.0);
    auto st = runAnalyzer (p, sr, 1440);
    CHECK (std::abs (st.result.lagSamples - 25.4) < 0.05 && st.result.polarity == -1,
           "measured %.3f pol %d", st.result.lagSamples, st.result.polarity);
    std::printf ("  measured %.3f (want 25.400), polarity %d, conf %.2f\n", st.result.lagSamples, st.result.polarity, st.result.confidence);
}

static void testNotSure()
{
    std::printf ("Unrelated tracks and silence are 'not sure', never applied\n");
    const double sr = 48000.0;
    const size_t n = (size_t) (20 * sr);
    Pair p { drums (n, sr, 5), drums (n, sr, 6) };
    auto st = runAnalyzer (p, sr, 1440);
    CHECK (st.state == sa::AlignAnalyzer::State::notSure, "unrelated tracks: state %d conf %.2f", (int) st.state, st.result.confidence);
    std::printf ("  unrelated: conf %.2f agree %.2f\n", st.result.confidence, st.result.agreement);

    Pair q { Vec (n, 0.0f), drums (n, sr, 7) };
    st = runAnalyzer (q, sr, 1440);
    CHECK (st.state == sa::AlignAnalyzer::State::notSure && st.result.windows == 0, "silent track: state %d, %d windows", (int) st.state, st.result.windows);
}

static void testAlternatePoints()
{
    std::printf ("Alternate points: a strong second arrival shows up as a second point\n");
    const double sr = 48000.0;
    const size_t n = (size_t) (14 * sr);
    Vec src = drums (n, sr, 21);
    Pair p;
    p.r = src; addTo (p.r, noise (n, 1, 0.0003), 1.0);
    p.x.assign (n, 0.0f);
    addTo (p.x, delayExact (src, 300.3), 0.7);
    addTo (p.x, delayExact (src, 300.3 + 400.0), -0.55);   // e.g. a reflection off a wall, inverted
    addTo (p.x, noise (n, 2, 0.0003), 1.0);
    auto st = runAnalyzer (p, sr, 1440);
    const auto& pts = st.result.points;
    CHECK (! pts.empty() && std::abs (pts[0].lagSamples - 300.3) < 0.05 && pts[0].polarity == 1, "main point %.3f", pts.empty() ? 0.0 : pts[0].lagSamples);
    bool second = false;
    for (size_t i = 1; i < pts.size(); ++i)
        if (std::abs (pts[i].lagSamples - 700.3) < 0.1 && pts[i].polarity == -1) second = true;
    CHECK (second, "second arrival at 700.3 (inverted) not among %zu points", pts.size());
    for (auto& pt : pts) std::printf ("  point %8.3f  pol %+d  strength %.2f\n", pt.lagSamples, pt.polarity, pt.strength);
}

static void testThreaded()
{
    std::printf ("Real background thread: listens while 'audio' is pushed, finishes by itself\n");
    const double sr = 48000.0;
    auto p = session (sr, 512.75, -1, 30.0, 99);
    sa::AlignAnalyzer a;
    a.prepare (sr, 1440);
    a.start();
    const int id0 = a.getStatus().id;
    const int block = 256;
    for (size_t pos = 0; pos + block <= p.x.size() && a.isListening(); pos += block)
    {
        a.push (p.x.data() + pos, p.r.data() + pos, block);
        if ((pos / block) % 8 == 0) std::this_thread::sleep_for (std::chrono::milliseconds (1));   // ~5x real time
    }
    for (int i = 0; i < 200 && a.getStatus().state == sa::AlignAnalyzer::State::listening; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    auto st = a.getStatus();
    CHECK (st.state == sa::AlignAnalyzer::State::done, "state %d", (int) st.state);
    CHECK (st.id == id0 + 1, "id %d -> %d", id0, st.id);
    CHECK (std::abs (st.result.lagSamples - 512.75) < 0.05 && st.result.polarity == -1, "measured %.3f pol %d", st.result.lagSamples, st.result.polarity);
    std::printf ("  finished after %.1f s of audio, lag %.3f\n", st.heardSeconds, st.result.lagSamples);

    // and again (a second ALIGN on the same analyzer)
    a.start();
    for (size_t pos = 0; pos + block <= p.x.size() && a.isListening(); pos += block)
    {
        a.push (p.x.data() + pos, p.r.data() + pos, block);
        if ((pos / block) % 8 == 0) std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    for (int i = 0; i < 300 && a.getStatus().state == sa::AlignAnalyzer::State::listening; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    CHECK (a.getStatus().id == id0 + 2, "second ALIGN didn't finish");
}

static void testPushNoAlloc()
{
    std::printf ("The audio thread's push never allocates (and drops instead of blocking when full)\n");
    sa::AlignAnalyzer a;
    a.prepare (48000.0, 1440);
    a.start();
    Vec x (512, 0.1f), r (512, 0.1f);
    gAllocCount = 0; gWatchAllocs = true;
    for (int i = 0; i < 3000; ++i) a.push (x.data(), r.data(), 512);   // more than the ring holds, nobody draining yet
    gWatchAllocs = false;
    CHECK (gAllocCount == 0, "%d allocations", gAllocCount.load());
    a.cancel();
}

int main()
{
    testAccuracy();
    testKickPair();
    testNotSure();
    testAlternatePoints();
    testThreaded();
    testPushNoAlloc();
    if (gFailures == 0) std::printf ("\nALL ANALYSIS TESTS PASSED\n");
    else                std::printf ("\n%d FAILURE(S)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
