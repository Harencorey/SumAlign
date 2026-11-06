#pragma once
//==============================================================================
// SumAlign - ALIGN: measures how far (and in which polarity) this track sits from a reference.
// Plain C++ (no JUCE), so it can move into The Gate later unchanged.
//
//   audio thread  --push-->  CaptureRing  --pop-->  analysis thread: PairEstimator  -->  Status
//
// PairEstimator (single-threaded, also used directly by the tests):
//   1. Cuts both signals into overlapping windows (Hann, ~4x the delay range long).
//   2. Keeps only windows where both tracks have real, transient-rich signal (hits),
//      judged on the first difference (+6 dB/oct) so a kick's low end doesn't dominate.
//   3. Accumulates the cross-spectrum X.R* and both power spectra over the kept windows.
//   4. Correlates with coherence weighting (frequencies the two mics really share count most;
//      bleed, noise and one-mic-only content count little) -> a sharp peak at the true lag.
//   5. Refines the peak to a fraction of a sample by evaluating the band-limited correlation
//      directly (exact interpolation), reads the polarity from its sign.
//   6. Confidence = how many single windows agree on that lag, and how far the peak stands out.
//   7. Lists other strong peaks ("alternate points") for the Earlier/Later buttons.
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
    double agreement = 0.0;         // 0..1: fraction of single windows that agree within 1.5 samples
    double peakRatio = 0.0;         // peak / rms of the rest of the correlation
    double confidence = 0.0;        // 0..1, what the UI shows
    int    windows = 0;             // windows (hits) used
    std::vector<AlignPoint> points; // strongest first is points[0] == the result; others sorted by strength
};

//------------------------------------------------------------------------------
class PairEstimator
{
public:
    // maxLag: the largest |lag| to search, in samples (the plug-in's delay range).
    void prepare (double sampleRate, int maxLagSamples)
    {
        sr = sampleRate;
        maxLag = std::max (1, maxLagSamples);
        win = 4096;
        while (win < 4 * maxLag) win <<= 1;
        hop = win / 2;
        nfft = 2 * win;
        fft.prepare (nfft);
        bufX.assign ((size_t) win, 0.0f);
        bufR.assign ((size_t) win, 0.0f);
        hann.resize ((size_t) win);
        for (int i = 0; i < win; ++i) hann[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / win);
        nb = nfft / 2 + 1;
        sxr.assign ((size_t) nb, cplx {});
        sxx.assign ((size_t) nb, 0.0);
        srr.assign ((size_t) nb, 0.0);
        fx.assign ((size_t) nfft, cplx {});
        fr.assign ((size_t) nfft, cplx {});
        work.assign ((size_t) nfft, cplx {});
        kLo = std::max (1, (int) std::ceil (40.0 * nfft / sr));
        kHi = std::min (nb - 1, (int) std::floor (std::min (18000.0, 0.45 * sr) * nfft / sr));
        reset();
    }

    void reset()
    {
        fill = 0;
        windows = 0;
        std::fill (sxr.begin(), sxr.end(), cplx {});
        std::fill (sxx.begin(), sxx.end(), 0.0);
        std::fill (srr.begin(), srr.end(), 0.0);
        perWindowLag.clear();
        maxEx = maxEr = 0.0;
    }

    int windowLength() const noexcept { return win; }
    int windowsUsed()  const noexcept { return windows; }

    // Feed time-aligned samples of this track (x) and the reference (r), as recorded.
    void feed (const float* x, const float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            bufX[(size_t) fill] = x[i];
            bufR[(size_t) fill] = r[i];
            if (++fill == win)
            {
                analyseWindow();
                // keep the second half for the next (50 % overlapping) window
                std::copy (bufX.begin() + hop, bufX.end(), bufX.begin());
                std::copy (bufR.begin() + hop, bufR.end(), bufR.begin());
                fill = win - hop;
            }
        }
    }

    AlignResult result()
    {
        AlignResult res;
        res.windows = windows;
        if (windows < 2) return res;

        // coherence-weighted cross-spectrum (one-sided -> Hermitian full spectrum)
        std::fill (work.begin(), work.end(), cplx {});
        weights.assign ((size_t) nb, cplx {});
        const double gMax = windows < 4 ? 0.9 : 0.98;
        for (int k = kLo; k <= kHi; ++k)
        {
            const double den = sxx[(size_t) k] * srr[(size_t) k];
            const double mag = std::abs (sxr[(size_t) k]);
            if (den <= 1e-30 || mag <= 1e-30) continue;
            double g2 = std::min (gMax, mag * mag / den);
            const double wgt = g2 / (1.0 - g2);
            weights[(size_t) k] = sxr[(size_t) k] / mag * wgt;
        }
        for (int k = 0; k < nb; ++k)
        {
            work[(size_t) k] = weights[(size_t) k];
            if (k > 0 && k < nfft / 2) work[(size_t) (nfft - k)] = std::conj (weights[(size_t) k]);
        }
        fft.run (work, true);   // c[n] = sum W_k e^{+j w n}

        // coarse correlation over -maxLag..+maxLag
        const int L = std::min (maxLag, nfft / 2 - 1);
        coarse.assign ((size_t) (2 * L + 1), 0.0);
        double best = -1.0; int bestI = 0;
        for (int t = -L; t <= L; ++t)
        {
            const double v = work[(size_t) ((t + nfft) % nfft)].real();
            coarse[(size_t) (t + L)] = v;
            if (std::abs (v) > best) { best = std::abs (v); bestI = t; }
        }
        if (best <= 0.0) return res;

        const double lag = refine ((double) bestI);
        const double peak = corrAt (lag);
        res.valid = true;
        res.lagSamples = lag;
        res.polarity = peak >= 0.0 ? 1 : -1;

        // peak to "everything else" (outside +-0.3 ms of the peak)
        const int guard = std::max (2, (int) std::lround (0.0003 * sr));
        double s2 = 0.0; int cnt = 0;
        for (int t = -L; t <= L; ++t)
            if (std::abs (t - bestI) > guard) { s2 += coarse[(size_t) (t + L)] * coarse[(size_t) (t + L)]; ++cnt; }
        const double rmsRest = cnt > 0 ? std::sqrt (s2 / cnt) : 1e-30;
        res.peakRatio = std::abs (peak) / std::max (rmsRest, 1e-30);

        int agree = 0;
        for (double l : perWindowLag) if (std::abs (l - lag) <= 1.5) ++agree;
        res.agreement = perWindowLag.empty() ? 0.0 : (double) agree / (double) perWindowLag.size();
        const double peakScore = std::clamp ((res.peakRatio - 4.0) / (12.0 - 4.0), 0.0, 1.0);
        res.confidence = std::min (res.agreement, peakScore);

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

private:
    void analyseWindow()
    {
        // hits only: both tracks need signal, and transient content near their loudest so far
        double ex = 0.0, er = 0.0, px = 0.0, pr = 0.0;
        for (int i = 1; i < win; ++i)
        {
            const double dx = (double) bufX[(size_t) i] - bufX[(size_t) (i - 1)];
            const double dr = (double) bufR[(size_t) i] - bufR[(size_t) (i - 1)];
            ex += dx * dx; er += dr * dr;
            px += (double) bufX[(size_t) i] * bufX[(size_t) i];
            pr += (double) bufR[(size_t) i] * bufR[(size_t) i];
        }
        maxEx = std::max (maxEx * 0.995, ex);
        maxEr = std::max (maxEr * 0.995, er);
        const double floor = 1e-8 * win;   // ~ -80 dBFS rms
        if (px < floor || pr < floor) return;
        if (ex < 0.05 * maxEx || er < 0.05 * maxEr) return;

        for (int i = 0; i < nfft; ++i)
        {
            const double w = i < win ? hann[(size_t) i] : 0.0;
            fx[(size_t) i] = i < win ? cplx (bufX[(size_t) i] * w, 0.0) : cplx {};
            fr[(size_t) i] = i < win ? cplx (bufR[(size_t) i] * w, 0.0) : cplx {};
        }
        fft.run (fx, false);
        fft.run (fr, false);

        // this window's own estimate (PHAT^0.8), for the agreement count
        std::fill (work.begin(), work.end(), cplx {});
        for (int k = kLo; k <= kHi; ++k)
        {
            const cplx c = fx[(size_t) k] * std::conj (fr[(size_t) k]);
            sxr[(size_t) k] += c;
            sxx[(size_t) k] += std::norm (fx[(size_t) k]);
            srr[(size_t) k] += std::norm (fr[(size_t) k]);
            const double m = std::abs (c);
            if (m > 1e-30)
            {
                const cplx w = c / std::pow (m, 0.8);
                work[(size_t) k] = w;
                if (k < nfft / 2) work[(size_t) (nfft - k)] = std::conj (w);
            }
        }
        fft.run (work, true);
        const int L = std::min (maxLag, nfft / 2 - 1);
        double best = -1.0; int bi = 0;
        for (int t = -L; t <= L; ++t)
        {
            const double v = std::abs (work[(size_t) ((t + nfft) % nfft)].real());
            if (v > best) { best = v; bi = t; }
        }
        // parabolic sub-sample
        auto at = [&] (int t) { return std::abs (work[(size_t) ((t + nfft) % nfft)].real()); };
        double frac = 0.0;
        if (bi > -L && bi < L)
        {
            const double a = at (bi - 1), b = at (bi), c = at (bi + 1);
            const double d = a - 2.0 * b + c;
            if (std::abs (d) > 1e-30) frac = std::clamp (0.5 * (a - c) / d, -0.5, 0.5);
        }
        perWindowLag.push_back (bi + frac);
        ++windows;
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
    int maxLag = 1440, win = 8192, hop = 4096, nfft = 16384, nb = 8193, kLo = 1, kHi = 8000;
    int fill = 0, windows = 0;
    double maxEx = 0.0, maxEr = 0.0;
    Fft fft;
    std::vector<float> bufX, bufR;
    std::vector<double> hann, sxx, srr, coarse;
    std::vector<cplx> sxr, fx, fr, work, weights;
    std::vector<double> perWindowLag;
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

    static constexpr int    kMinWindows    = 24;     // hits before it may finish on its own (~4-5 s of drums)
    static constexpr double kDoneAgreement = 0.7;
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

        const bool confident = r.valid && r.windows >= kMinWindows && r.agreement >= kDoneAgreement && r.confidence >= 0.6 && stableCount >= 2;
        const bool timeUp = status.heardSeconds >= kMaxSeconds;
        // playback stopped (~1.5 s without audio) and it already has a good answer: finish
        const bool stoppedButSure = quietPasses >= 60 && r.valid && r.windows >= kMinWindows && r.confidence >= 0.6;
        if (confident || stopReq || timeUp || stoppedButSure)
        {
            listening.store (false, std::memory_order_release);
            const bool ok = r.valid && r.confidence >= 0.5 && r.windows >= 4;
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
