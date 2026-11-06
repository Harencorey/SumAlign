#pragma once
//==============================================================================
// SumAlign - ALIGN: measures how far (and in which polarity) this track sits from a reference.
// Plain C++ (no JUCE), so it can move into The Gate later unchanged.
//
//   audio thread  --push-->  CaptureRing  --pop-->  analysis thread: PairEstimator  -->  Status
//
// PairEstimator (single-threaded, also used directly by the tests):
//   1. Finds the reference's own hits (its transient envelope rising through 30 % of its recent
//      loud-hit level, 80 ms apart at most) and takes one window per hit, starting 10 ms + the
//      delay range before it, ~4x the delay range long (Hann).
//   2. Accumulates the cross-spectrum X.R* over all hits, and separately in 6 round-robin groups.
//   3. Correlation, half-whitened (cross-spectrum divided by the 4th root of both power spectra)
//      and A-weighted. Chosen on Corey's 7-song drum session against plain, +6 dB/oct, A-weighted,
//      PHAT, SCOT and coherence (ML) weighting: the only one that gave the same answer song after
//      song on every snare pair AND a physically sensible one (snare bottom 0.45 ms, inverted).
//      Plain/A-weighted locks onto drum ringing; full whitening falls apart on overheads.
//   4. Refines the peak to a fraction of a sample by evaluating the band-limited correlation
//      directly (exact interpolation), reads the polarity from its sign.
//   5. "Sure" = how many of the 6 groups, on their own, land on the same lag (within 2 samples)
//      and polarity. Pairs that don't really share a sound (or are ambiguous) disagree.
//   6. Lists other strong peaks ("alternate points") for the Earlier/Later buttons.
//
// Sign convention: lagSamples > 0 means this track hears the source LATER than the reference,
// so the correction is Delay = -lagSamples. polarity -1 means it's inverted relative to the reference.
//==============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace sa
{

using cplx = std::complex<double>;

//------------------------------------------------------------------------------
// In-place iterative radix-2 FFT. inverse = true gives the unscaled inverse.
class Fft
{
public:
    void prepare (int n)
    {
        size = n;
        bits = 0; while ((1 << bits) < n) ++bits;
        rev.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        tw.resize ((size_t) (n / 2));
        for (int i = 0; i < n / 2; ++i)
            tw[(size_t) i] = std::polar (1.0, -2.0 * 3.14159265358979323846 * i / n);
    }

    int length() const noexcept { return size; }

    void run (std::vector<cplx>& a, bool inverse) const
    {
        const int n = size;
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i]) std::swap (a[(size_t) i], a[(size_t) rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    cplx w = tw[(size_t) (j * step)];
                    if (inverse) w = std::conj (w);
                    const cplx u = a[(size_t) (i + j)], v = a[(size_t) (i + j + half)] * w;
                    a[(size_t) (i + j)] = u + v;
                    a[(size_t) (i + j + half)] = u - v;
                }
        }
    }

private:
    int size = 0, bits = 0;
    std::vector<int> rev;
    std::vector<cplx> tw;
};

//------------------------------------------------------------------------------
struct AlignPoint
{
    double lagSamples = 0.0;   // see the sign convention above
    int    polarity = 1;       // +1 normal, -1 inverted
    double strength = 0.0;     // 0..1 relative to the strongest point
};

struct AlignResult
{
    bool   valid = false;           // there's a measurement at all
    double lagSamples = 0.0;
    int    polarity = 1;
    double agreement = 0.0;         // 0..1: fraction of the 6 hit groups that land on the same lag/polarity
    double peakRatio = 0.0;         // peak / rms of the rest of the correlation
    double confidence = 0.0;        // 0..1, what the UI shows
    int    windows = 0;             // windows (hits) used
    std::vector<AlignPoint> points; // strongest first is points[0] == the result; others sorted by strength
};

//------------------------------------------------------------------------------
class PairEstimator
{
public:
    static constexpr int kSubsets = 6;   // hits are dealt round-robin into 6 groups; "sure" = how many groups agree

    // maxLag: the largest |lag| to search, in samples (the plug-in's delay range).
    void prepare (double sampleRate, int maxLagSamples)
    {
        sr = sampleRate;
        maxLag = std::max (1, maxLagSamples);
        win = 4096;
        while (win < 4 * maxLag) win <<= 1;
        nfft = 2 * win;
        fft.prepare (nfft);
        hist = 1; while (hist < 2 * win + (int) (0.2 * sr)) hist <<= 1;
        histX.assign ((size_t) hist, 0.0f);
        histR.assign ((size_t) hist, 0.0f);
        hann.resize ((size_t) win);
        for (int i = 0; i < win; ++i) hann[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / win);
        nb = nfft / 2 + 1;
        kLo = std::max (1, (int) std::ceil (30.0 * nfft / sr));
        kHi = std::min (nb - 1, (int) std::floor (std::min (18000.0, 0.45 * sr) * nfft / sr));
        aw.assign ((size_t) nb, 0.0);
        for (int k = kLo; k <= kHi; ++k) aw[(size_t) k] = aWeightPower ((double) k * sr / nfft);
        total.assign ((size_t) nb, cplx {});
        for (auto& v : sub) v.assign ((size_t) nb, cplx {});
        pxx.assign ((size_t) nb, 0.0); prr.assign ((size_t) nb, 0.0);
        for (auto& v : subXX) v.assign ((size_t) nb, 0.0);
        for (auto& v : subRR) v.assign ((size_t) nb, 0.0);
        fx.assign ((size_t) nfft, cplx {});
        fr.assign ((size_t) nfft, cplx {});
        work.assign ((size_t) nfft, cplx {});
        holdDecay  = std::exp (-1.0 / (0.002 * sr));   // 2 ms peak hold on the reference's transient envelope
        levelDecay = std::exp (-1.0 / (3.0 * sr));     // loud-hit level, forgets over ~3 s
        refractory = (long long) (0.08 * sr);
        preRoll    = (long long) (0.010 * sr) + maxLag; // window starts 10 ms + the delay range before the hit
        reset();
    }

    void reset()
    {
        n = 0; windows = 0; nPending = 0;
        lastOnset = -(1LL << 40);
        env = level = 0.0; prevR = 0.0f; above = false;
        std::fill (total.begin(), total.end(), cplx {});
        for (auto& v : sub) std::fill (v.begin(), v.end(), cplx {});
        std::fill (pxx.begin(), pxx.end(), 0.0); std::fill (prr.begin(), prr.end(), 0.0);
        for (auto& v : subXX) std::fill (v.begin(), v.end(), 0.0);
        for (auto& v : subRR) std::fill (v.begin(), v.end(), 0.0);
        std::fill (histX.begin(), histX.end(), 0.0f);
        std::fill (histR.begin(), histR.end(), 0.0f);
    }

    int windowLength() const noexcept { return win; }
    int windowsUsed()  const noexcept { return windows; }

    // Feed time-aligned samples of this track (x) and the reference (r), as recorded.
    void feed (const float* x, const float* r, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            histX[(size_t) (n & (hist - 1))] = x[i];
            histR[(size_t) (n & (hist - 1))] = r[i];

            // hits of the reference: its transient envelope (first difference, 2 ms hold) rising
            // through 30 % of its recent loud-hit level
            const double d = std::abs ((double) r[i] - prevR);
            prevR = r[i];
            env = std::max (d, env * holdDecay);
            level = std::max (env, level * levelDecay);
            const bool on = env > 0.3 * level && level > 1e-5;
            if (on && ! above && n - lastOnset > refractory && nPending < (int) pending.size())
            {
                pending[(size_t) nPending++] = n - preRoll;
                lastOnset = n;
            }
            above = on;
            ++n;

            // windows whose audio is complete
            while (nPending > 0 && n >= pending[0] + win)
            {
                const long long s = pending[0];
                for (int k = 1; k < nPending; ++k) pending[(size_t) (k - 1)] = pending[(size_t) k];
                --nPending;
                if (s >= 0 && n - s <= hist) analyseWindow (s);
            }
        }
    }

    AlignResult result()
    {
        AlignResult res;
        res.windows = windows;
        if (windows < 2) return res;

        weights.assign ((size_t) nb, cplx {});
        for (int k = kLo; k <= kHi; ++k) weights[(size_t) k] = weigh (total[(size_t) k], pxx[(size_t) k], prr[(size_t) k], k);
        const int L = std::min (maxLag, nfft / 2 - 1);
        correlate (weights, L, coarse);
        int bestI = 0; double best = -1.0;
        for (int t = -L; t <= L; ++t)
            if (std::abs (coarse[(size_t) (t + L)]) > best) { best = std::abs (coarse[(size_t) (t + L)]); bestI = t; }
        if (best <= 0.0) return res;

        const double lag = refine ((double) bestI);
        const double peak = corrAt (lag);
        res.valid = true;
        res.lagSamples = lag;
        res.polarity = peak >= 0.0 ? 1 : -1;

        // strongest other peak (outside +-0.3 ms): how much the answer stands out
        const int guard = std::max (2, (int) std::lround (0.0003 * sr));
        double second = 0.0;
        for (int t = -L + 1; t < L; ++t)
        {
            const double a = std::abs (coarse[(size_t) (t + L)]);
            if (std::abs (t - bestI) > guard && a >= std::abs (coarse[(size_t) (t + L - 1)]) && a >= std::abs (coarse[(size_t) (t + L + 1)]))
                second = std::max (second, a);
        }
        res.peakRatio = second > 0.0 ? best / second : 10.0;

        // the 6 groups of hits, each on its own: same lag (within 2 samples) and same polarity?
        int agree = 0, groups = 0;
        std::vector<double> sc;
        for (int g = 0; g < kSubsets; ++g)
        {
            subW.assign ((size_t) nb, cplx {});
            double e = 0.0;
            for (int k = kLo; k <= kHi; ++k)
            {
                subW[(size_t) k] = weigh (sub[(size_t) g][(size_t) k], subXX[(size_t) g][(size_t) k], subRR[(size_t) g][(size_t) k], k);
                e += std::abs (subW[(size_t) k]);
            }
            if (e <= 0.0) continue;
            ++groups;
            correlate (subW, L, sc);
            int bi = 0; double bv = -1.0;
            for (int t = -L; t <= L; ++t) if (std::abs (sc[(size_t) (t + L)]) > bv) { bv = std::abs (sc[(size_t) (t + L)]); bi = t; }
            const int pol = sc[(size_t) (bi + L)] >= 0.0 ? 1 : -1;
            if (std::abs ((double) bi - lag) <= 2.0 && pol == res.polarity) ++agree;
        }
        res.agreement = (double) agree / (double) kSubsets;
        res.confidence = windows >= 2 * kSubsets ? res.agreement : 0.0;

        // alternate points: other strong local maxima of |c|, at least 0.25 ms apart
        res.points.push_back ({ lag, res.polarity, 1.0 });
        const int sep = std::max (2, (int) std::lround (0.00025 * sr));
        std::vector<int> cand;
        for (int t = -L + 1; t < L; ++t)
        {
            const double a = std::abs (coarse[(size_t) (t + L)]);
            if (a >= 0.35 * best && a >= std::abs (coarse[(size_t) (t + L - 1)]) && a >= std::abs (coarse[(size_t) (t + L + 1)]))
                cand.push_back (t);
        }
        std::sort (cand.begin(), cand.end(), [&] (int a, int b)
                   { return std::abs (coarse[(size_t) (a + L)]) > std::abs (coarse[(size_t) (b + L)]); });
        for (int t : cand)
        {
            bool farEnough = true;
            for (auto& p : res.points) if (std::abs ((double) t - p.lagSamples) < sep) { farEnough = false; break; }
            if (! farEnough) continue;
            const double l2 = refine ((double) t);
            const double v2 = corrAt (l2);
            res.points.push_back ({ l2, v2 >= 0.0 ? 1 : -1, std::min (1.0, std::abs (v2) / std::abs (peak)) });
            if (res.points.size() >= 8) break;
        }
        return res;
    }

    // A-weighting (IEC 61672), as power: the sum is judged by how loud it sounds
    static double aWeightPower (double f)
    {
        const double f2 = f * f;
        const double ra = (12194.0 * 12194.0 * f2 * f2)
                        / ((f2 + 20.6 * 20.6) * std::sqrt ((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
        return ra * ra;
    }

private:
    // half-whitened (divide by the 4th root of both power spectra: ringing drum tones no longer
    // swamp the timing information, without blowing up noise like full whitening does), then A-weighted
    cplx weigh (cplx sxr, double xx, double rr, int k) const
    {
        const double m = std::sqrt (std::sqrt (xx * rr));
        return m > 1e-30 ? sxr / m * aw[(size_t) k] : cplx {};
    }

    void analyseWindow (long long s)
    {
        // both tracks need signal in this window (~ -80 dBFS rms), or it isn't a shared hit
        double ex = 0.0, er = 0.0;
        for (int i = 0; i < win; ++i)
        {
            const size_t h = (size_t) ((s + i) & (hist - 1));
            ex += (double) histX[h] * histX[h];
            er += (double) histR[h] * histR[h];
        }
        if (ex < 1e-8 * win || er < 1e-8 * win) return;

        for (int i = 0; i < nfft; ++i)
        {
            if (i < win)
            {
                const size_t h = (size_t) ((s + i) & (hist - 1));
                fx[(size_t) i] = cplx (histX[h] * hann[(size_t) i], 0.0);
                fr[(size_t) i] = cplx (histR[h] * hann[(size_t) i], 0.0);
            }
            else fx[(size_t) i] = fr[(size_t) i] = cplx {};
        }
        fft.run (fx, false);
        fft.run (fr, false);
        const size_t gi = (size_t) (windows % kSubsets);
        auto& g = sub[gi]; auto& gx = subXX[gi]; auto& gr = subRR[gi];
        for (int k = kLo; k <= kHi; ++k)
        {
            const cplx c = fx[(size_t) k] * std::conj (fr[(size_t) k]);
            const double xx = std::norm (fx[(size_t) k]), rr = std::norm (fr[(size_t) k]);
            total[(size_t) k] += c; pxx[(size_t) k] += xx; prr[(size_t) k] += rr;
            g[(size_t) k] += c; gx[(size_t) k] += xx; gr[(size_t) k] += rr;
        }
        ++windows;
    }

    // real correlation c[t] = sum W_k e^{+j w t} for t in -L..L
    void correlate (const std::vector<cplx>& w, int L, std::vector<double>& out)
    {
        std::fill (work.begin(), work.end(), cplx {});
        for (int k = 0; k < nb; ++k)
        {
            work[(size_t) k] = w[(size_t) k];
            if (k > 0 && k < nfft / 2) work[(size_t) (nfft - k)] = std::conj (w[(size_t) k]);
        }
        fft.run (work, true);
        out.assign ((size_t) (2 * L + 1), 0.0);
        for (int t = -L; t <= L; ++t) out[(size_t) (t + L)] = work[(size_t) ((t + nfft) % nfft)].real();
    }

    // band-limited correlation at a fractional lag (exact interpolation of the weighted spectrum)
    double corrAt (double tau) const
    {
        double acc = 0.0;
        const double w0 = 2.0 * 3.14159265358979323846 / nfft;
        for (int k = kLo; k <= kHi; ++k)
        {
            const cplx& w = weights[(size_t) k];
            if (w == cplx {}) continue;
            const double ph = w0 * k * tau;
            acc += 2.0 * (w.real() * std::cos (ph) - w.imag() * std::sin (ph));
        }
        return acc;
    }

    // golden-section search for the |peak| within +-0.6 samples of t0
    double refine (double t0) const
    {
        const double g = 0.6180339887498949;
        double a = t0 - 0.6, b = t0 + 0.6;
        double c = b - g * (b - a), d = a + g * (b - a);
        double fc = std::abs (corrAt (c)), fd = std::abs (corrAt (d));
        for (int it = 0; it < 40; ++it)
        {
            if (fc > fd) { b = d; d = c; fd = fc; c = b - g * (b - a); fc = std::abs (corrAt (c)); }
            else         { a = c; c = d; fc = fd; d = a + g * (b - a); fd = std::abs (corrAt (d)); }
        }
        return 0.5 * (a + b);
    }

    double sr = 48000.0;
    int maxLag = 1440, win = 8192, nfft = 16384, nb = 8193, kLo = 1, kHi = 8000, hist = 32768;
    long long n = 0, lastOnset = 0, refractory = 3840, preRoll = 1920;
    int windows = 0, nPending = 0;
    double env = 0.0, level = 0.0, holdDecay = 0.99, levelDecay = 0.9999;
    float prevR = 0.0f;
    bool above = false;
    std::array<long long, 64> pending {};
    Fft fft;
    std::vector<float> histX, histR;
    std::vector<double> hann, aw, coarse, pxx, prr;
    std::array<std::vector<double>, kSubsets> subXX, subRR;
    std::vector<cplx> total, fx, fr, work, weights, subW;
    std::array<std::vector<cplx>, kSubsets> sub;
};

//------------------------------------------------------------------------------
// Single-producer (audio thread) / single-consumer (analysis thread) ring of sample pairs.
// The audio thread never blocks or allocates; if the ring is full, samples are dropped (counted).
class CaptureRing
{
public:
    void prepare (int capacityPow2)
    {
        cap = 1; while (cap < capacityPow2) cap <<= 1;
        xs.assign ((size_t) cap, 0.0f);
        rs.assign ((size_t) cap, 0.0f);
        clear();
    }
    void clear() noexcept { w.store (0); r.store (0); dropped.store (0); }

    // audio thread
    void push (const float* x, const float* ref, int n) noexcept
    {
        const size_t wi = w.load (std::memory_order_relaxed);
        const size_t ri = r.load (std::memory_order_acquire);
        const size_t room = (size_t) cap - (wi - ri);
        const size_t m = std::min ((size_t) n, room);
        for (size_t i = 0; i < m; ++i)
        {
            xs[(wi + i) & (size_t) (cap - 1)] = x[i];
            rs[(wi + i) & (size_t) (cap - 1)] = ref[i];
        }
        if (m < (size_t) n) dropped.fetch_add ((int) ((size_t) n - m), std::memory_order_relaxed);
        w.store (wi + m, std::memory_order_release);
    }

    // analysis thread: up to maxN pairs into x/r, returns how many
    int pop (float* x, float* ref, int maxN) noexcept
    {
        const size_t ri = r.load (std::memory_order_relaxed);
        const size_t wi = w.load (std::memory_order_acquire);
        const size_t m = std::min ((size_t) maxN, wi - ri);
        for (size_t i = 0; i < m; ++i)
        {
            x[i]   = xs[(ri + i) & (size_t) (cap - 1)];
            ref[i] = rs[(ri + i) & (size_t) (cap - 1)];
        }
        r.store (ri + m, std::memory_order_release);
        return (int) m;
    }

    std::atomic<int> dropped { 0 };

private:
    int cap = 0;
    std::vector<float> xs, rs;
    std::atomic<size_t> w { 0 }, r { 0 };
};

//------------------------------------------------------------------------------
// The ALIGN session: runs PairEstimator on a background thread while the song plays.
class AlignAnalyzer
{
public:
    enum class State { idle, listening, done, notSure };

    struct Status
    {
        State state = State::idle;
        double progress = 0.0;       // 0..1 while listening
        double heardSeconds = 0.0;   // audio analysed so far
        AlignResult result;          // latest (while listening) or final
        int id = 0;                  // increments with every finished ALIGN (so a result is applied once)
    };

    static constexpr int    kMinWindows    = 36;     // hits before it may finish on its own
    static constexpr double kDoneAgreement = 0.83;   // 5 of the 6 groups agree
    static constexpr double kMaxSeconds    = 120.0;  // stops listening after this much audio

    ~AlignAnalyzer() { shutdown(); }

    // message thread: before any audio processing at this rate
    void prepare (double sampleRate, int maxLagSamples)
    {
        std::lock_guard<std::mutex> lk (mtx);
        sr = sampleRate;
        maxLag = maxLagSamples;
        ring.prepare (1 << 19);   // ~11 s at 48 kHz of slack for the analysis thread
        status = Status { State::idle, 0.0, 0.0, {}, status.id };
        listening.store (false);
        configured = false;
    }

    // false = no background thread; the owner calls pump() itself (tests, offline tools)
    void setThreaded (bool t) { threaded = t; }

    // message thread
    void start()
    {
        std::unique_lock<std::mutex> lk (mtx);
        if (threaded) ensureThread();
        listening.store (false, std::memory_order_release);
        cv.wait (lk, [this] { return ! busy; });   // the worker isn't in the middle of the old analysis
        est.prepare (sr, maxLag);
        heard = 0;
        stableCount = 0;
        quietPasses = 0;
        lastLag = 1e9;
        ring.clear();
        status.state = State::listening;
        status.progress = 0.0;
        status.heardSeconds = 0.0;
        status.result = {};
        configured = true;
        listening.store (true, std::memory_order_release);
        cv.notify_all();
    }

    // message thread: stop now and use what has been heard
    void finishNow()
    {
        std::lock_guard<std::mutex> lk (mtx);
        if (status.state == State::listening) stopFlag = true;
        cv.notify_all();
    }

    void cancel()
    {
        listening.store (false);
        std::lock_guard<std::mutex> lk (mtx);
        status.state = State::idle;
        status.progress = 0.0;
    }

    // audio thread: cheap check + push
    bool isListening() const noexcept { return listening.load (std::memory_order_acquire); }
    void push (const float* x, const float* ref, int n) noexcept { if (isListening()) ring.push (x, ref, n); }

    Status getStatus() const
    {
        std::lock_guard<std::mutex> lk (mtx);
        return status;
    }

    // unthreaded mode: analyse whatever is in the ring now
    void pump() { std::unique_lock<std::mutex> lk (mtx); drainLocked (lk); }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lk (mtx);
            quit = true;
            listening.store (false);
            cv.notify_all();
        }
        if (worker.joinable()) worker.join();
    }

private:
    void ensureThread()
    {
        if (! worker.joinable())
            worker = std::thread ([this] { loop(); });
    }

    void loop()
    {
        std::unique_lock<std::mutex> lk (mtx);
        while (! quit)
        {
            cv.wait_for (lk, std::chrono::milliseconds (25));
            if (quit) break;
            if (status.state == State::listening && configured) drainLocked (lk);
        }
    }

    // called with the lock held; releases it around the heavy work
    void drainLocked (std::unique_lock<std::mutex>& lk)
    {
        if (status.state != State::listening || busy) return;
        bool stopReq = stopFlag; stopFlag = false;
        busy = true;
        lk.unlock();

        chunkX.resize (8192); chunkR.resize (8192);
        int got = 0, total = 0;
        const int before = est.windowsUsed();
        const int budget = (int) (0.5 * sr);   // at most 0.5 s of audio per pass, so the result is re-checked often
        while (total < budget && (got = ring.pop (chunkX.data(), chunkR.data(), std::min (8192, budget - total))) > 0)
        {
            est.feed (chunkX.data(), chunkR.data(), got);
            total += got;
        }
        if (total > 0) quietPasses = 0; else ++quietPasses;
        heard += total;
        AlignResult res;
        const bool newWindows = est.windowsUsed() != before;
        if (newWindows || stopReq) res = est.result();

        lk.lock();
        busy = false;
        cv.notify_all();
        if (status.state != State::listening) return;   // cancelled meanwhile
        status.heardSeconds = (double) heard / sr;
        if (newWindows || stopReq)
        {
            status.result = res;
            if (res.valid && std::abs (res.lagSamples - lastLag) < 0.1) ++stableCount; else stableCount = 0;
            lastLag = res.valid ? res.lagSamples : 1e9;
        }
        const auto& r = status.result;
        const double byHits = std::min (1.0, (double) r.windows / kMinWindows);
        status.progress = std::max (status.progress, std::min (0.99, byHits * std::max (0.25, r.agreement / kDoneAgreement)));

        const bool confident = r.valid && r.windows >= kMinWindows && r.confidence >= kDoneAgreement && stableCount >= 2;
        const bool timeUp = status.heardSeconds >= kMaxSeconds;
        // playback stopped (~1.5 s without audio) and it already has a good answer: finish
        const bool stoppedButSure = quietPasses >= 60 && r.valid && r.windows >= 2 * PairEstimator::kSubsets && r.confidence >= 0.66;
        if (confident || stopReq || timeUp || stoppedButSure)
        {
            listening.store (false, std::memory_order_release);
            const bool ok = r.valid && r.confidence >= 0.66;   // at least 4 of the 6 groups agree
            status.state = ok ? State::done : State::notSure;
            status.progress = 1.0;
            ++status.id;
        }
    }

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false, stopFlag = false, configured = false, busy = false, threaded = true;
    std::atomic<bool> listening { false };

    double sr = 48000.0;
    int maxLag = 1440;
    long long heard = 0;
    int stableCount = 0, quietPasses = 0;
    double lastLag = 1e9;
    CaptureRing ring;
    PairEstimator est;
    std::vector<float> chunkX, chunkR;
    Status status;
};

} // namespace sa
