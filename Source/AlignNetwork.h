#pragma once
//==============================================================================
// SumAlign - whole-session ALIGN (Phase 3). Plain C++ (no JUCE), so it can move into The Gate later.
//
//   NetworkEstimator: every track fed in sync -> every track's own hits -> each hit's window FFT'd for
//                     all tracks -> a cross-spectrum per directed pair (i's hits, j relative to i)
//                     -> per pair: lag, polarity, "sure" (the same method Phase 2 proved on real drums:
//                     hit-locked windows, half-whitened + A-weighted correlation, 6-group agreement).
//   solveSession:     groups grown from a root by majority vote, each track placed by its own sound as the
//                     others hear it; split votes are left alone (see the comment at solveSession).
//
// Sign convention as in AlignAnalysis.h: a pair lag > 0 means the second track hears it LATER.
// A track's Delay = -(its arrival - the anchor's arrival): later tracks move earlier.
//==============================================================================

#include "AlignAnalysis.h"
#include "AlignNames.h"
#include <functional>
#include <numeric>

namespace sa
{

struct PairEdge
{
    int a = 0, b = 0;           // a < b
    double lag = 0.0;           // b relative to a, samples
    int polarity = 1;           // b relative to a
    double sure = 0.0;          // 0..1 (6-group agreement)
    int hits = 0;
    double prominence = 1.0;    // main peak / strongest other peak (> 1; higher = less ambiguous)
};

// why a track was left alone
enum class LeftAlone { no = 0, nothingShared, disagree, notWithGroup, off, noAudio };

struct TrackAlign
{
    int group = -1;             // -1 = ungrouped: left alone (see why)
    int letter = -1;            // the group's letter shown to the user (0 = A); a manual group keeps its own
    LeftAlone why = LeftAlone::nothingShared;
    double delaySamples = 0.0;  // what to set Delay to
    int polarity = 1;           // what to set Polarity to (+1 normal, -1 inverted), relative to the anchor
    double sure = 0.0;          // weakest link between this track and the anchor
    bool anchor = false;
    bool clamped = false;       // the solution needed more than the delay range
};

//------------------------------------------------------------------------------
class NetworkEstimator
{
public:
    static constexpr int kSubsets = 6;
    static constexpr int kSubDecim = 4;   // group spectra keep every 4th bin (coarse lag check only)

    void prepare (double sampleRate, int maxLagSamples, int numTracks)
    {
        sr = sampleRate;
        maxLag = std::max (1, maxLagSamples);
        N = std::max (0, numTracks);
        win = 4096;
        while (win < 4 * maxLag) win <<= 1;
        nfft = 2 * win;
        nfftSub = nfft / kSubDecim;
        fft.prepare (nfft);
        fftSub.prepare (nfftSub);
        hist = 1; while (hist < 2 * win + (int) (0.3 * sr)) hist <<= 1;
        hann.resize ((size_t) win);
        for (int i = 0; i < win; ++i) hann[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / win);
        nb = nfft / 2 + 1;
        kLo = std::max (1, (int) std::ceil (30.0 * nfft / sr));
        kHi = std::min (nb - 1, (int) std::floor (std::min (18000.0, 0.45 * sr) * nfft / sr));
        nbins = kHi - kLo + 1;
        aw.assign ((size_t) nbins, 0.0);
        for (int k = kLo; k <= kHi; ++k) aw[(size_t) (k - kLo)] = PairEstimator::aWeightPower ((double) k * sr / nfft);

        tracks.assign ((size_t) N, Track {});
        for (auto& t : tracks) { t.hist.assign ((size_t) hist, 0.0f); }
        pairs.clear();
        pairs.resize ((size_t) (N * N));
        power.assign ((size_t) (N * N), {});
        spec.assign ((size_t) N, std::vector<cplx> ((size_t) nbins));
        buf.assign ((size_t) nfft, cplx {});
        work.assign ((size_t) nfft, cplx {});
        workSub.assign ((size_t) nfftSub, cplx {});
        holdDecay  = std::exp (-1.0 / (0.002 * sr));
        levelDecay = std::exp (-1.0 / (3.0 * sr));
        refractory = (long long) (0.15 * sr);   // at most ~6 windows a second per track (keeps the CPU sane)
        preRoll    = (long long) (0.010 * sr) + maxLag;
        reset();
    }

    void reset()
    {
        n = 0; nPending = 0;
        for (auto& t : tracks)
        {
            std::fill (t.hist.begin(), t.hist.end(), 0.0f);
            t.env = t.level = 0.0; t.prev = 0.0f; t.above = false; t.lastOnset = -(1LL << 40); t.hits = 0;
        }
        for (auto& p : pairs) p = Pair {};
        for (auto& p : power) p.clear();
    }

    int numTracks() const noexcept { return N; }
    int hitsOf (int i) const noexcept { return tracks[(size_t) i].hits; }
    int totalHits() const noexcept { int s = 0; for (auto& t : tracks) s += t.hits; return s; }

    // A gap or jump in the audio (transport loop, stop): pending windows would straddle it, so drop them.
    void breakStream() { nPending = 0; for (auto& t : tracks) { t.above = false; t.lastOnset = n; } }

    // Feed n frames of all tracks, time-aligned. x[i] may be nullptr = silence for that track.
    void feed (const float* const* x, int count)
    {
        for (int s = 0; s < count; ++s)
        {
            const size_t h = (size_t) (n & (hist - 1));
            for (int i = 0; i < N; ++i)
            {
                auto& t = tracks[(size_t) i];
                const float v = x[i] != nullptr ? x[i][s] : 0.0f;
                t.hist[h] = v;
                const double d = std::abs ((double) v - t.prev);
                t.prev = v;
                t.env = std::max (d, t.env * holdDecay);
                t.level = std::max (t.env, t.level * levelDecay);
                const bool on = t.env > 0.3 * t.level && t.level > 1e-5;
                if (on && ! t.above && n - t.lastOnset > refractory && nPending < (int) pending.size())
                {
                    pending[(size_t) nPending++] = { n - preRoll, i };
                    t.lastOnset = n;
                }
                t.above = on;
            }
            ++n;
            while (nPending > 0 && n >= pending[0].start + win)
            {
                const auto p = pending[0];
                for (int k = 1; k < nPending; ++k) pending[(size_t) (k - 1)] = pending[(size_t) k];
                --nPending;
                if (p.start >= 0 && n - p.start <= hist) analyseWindow (p.start, p.owner);
            }
        }
    }

    // Every directed estimate: a = whose hits were used, b relative to a.
    std::vector<PairEdge> directedEdges()
    {
        std::vector<PairEdge> out;
        for (int a = 0; a < N; ++a)
            for (int b = 0; b < N; ++b)
            {
                if (a == b) continue;
                PairEdge e;
                if (estimate (a, b, e)) { e.a = a; e.b = b; out.push_back (e); }
            }
        return out;
    }

    // Best estimate per track pair (the more confident of the two directions).
    std::vector<PairEdge> edges()
    {
        std::vector<PairEdge> out;
        for (int a = 0; a < N; ++a)
            for (int b = a + 1; b < N; ++b)
            {
                PairEdge e1, e2;
                const bool h1 = estimate (a, b, e1);   // a's hits, b relative to a
                const bool h2 = estimate (b, a, e2);   // b's hits, a relative to b
                if (! h1 && ! h2) continue;
                PairEdge e;
                e.a = a; e.b = b;
                const bool use1 = h1 && (! h2 || e1.sure > e2.sure || (e1.sure == e2.sure && e1.hits >= e2.hits));
                if (use1) { e.lag = e1.lag; e.polarity = e1.polarity; e.sure = e1.sure; e.hits = e1.hits; e.prominence = e1.prominence; }
                else      { e.lag = -e2.lag; e.polarity = e2.polarity; e.sure = e2.sure; e.hits = e2.hits; e.prominence = e2.prominence; }
                out.push_back (e);
            }
        return out;
    }

private:
    struct Track
    {
        std::vector<float> hist;
        double env = 0.0, level = 0.0;
        float prev = 0.0f;
        bool above = false;
        long long lastOnset = 0;
        int hits = 0;
    };
    struct Pair
    {
        std::vector<cplx> total;                                    // over the owner's hits
        std::array<std::vector<std::complex<float>>, kSubsets> sub; // every kSubDecim-th bin
        int hits = 0;
    };
    struct Pending { long long start = 0; int owner = 0; };

    void spectrum (int track, long long s, std::vector<cplx>& out)
    {
        const auto& hb = tracks[(size_t) track].hist;
        for (int i = 0; i < nfft; ++i)
            buf[(size_t) i] = i < win ? cplx (hb[(size_t) ((s + i) & (hist - 1))] * hann[(size_t) i], 0.0) : cplx {};
        fft.run (buf, false);
        for (int k = kLo; k <= kHi; ++k) out[(size_t) (k - kLo)] = buf[(size_t) k];
    }

    void analyseWindow (long long s, int owner)
    {
        // which tracks have signal in this window
        std::vector<char> active ((size_t) N, 0);
        for (int i = 0; i < N; ++i)
        {
            const auto& hb = tracks[(size_t) i].hist;
            double e = 0.0;
            for (int k = 0; k < win; ++k) { const float v = hb[(size_t) ((s + k) & (hist - 1))]; e += (double) v * v; }
            active[(size_t) i] = e >= 1e-8 * win;
        }
        if (! active[(size_t) owner]) return;
        int others = 0;
        for (int j = 0; j < N; ++j) if (j != owner && active[(size_t) j]) ++others;
        if (others == 0) return;

        auto& t = tracks[(size_t) owner];
        const int g = t.hits % kSubsets;
        ++t.hits;
        spectrum (owner, s, spec[(size_t) owner]);
        const auto& R = spec[(size_t) owner];
        auto& pRR = power[(size_t) (owner * N + owner)];
        if (pRR.empty()) pRR.assign ((size_t) nbins, 0.0);
        for (int k = 0; k < nbins; ++k) pRR[(size_t) k] += std::norm (R[(size_t) k]);

        for (int j = 0; j < N; ++j)
        {
            if (j == owner || ! active[(size_t) j]) continue;
            spectrum (j, s, spec[(size_t) j]);
            const auto& X = spec[(size_t) j];
            auto& p = pairs[(size_t) (owner * N + j)];
            if (p.total.empty())
            {
                p.total.assign ((size_t) nbins, cplx {});
                for (auto& v : p.sub) v.assign ((size_t) (nfftSub / 2 + 1), std::complex<float> {});
            }
            auto& pXX = power[(size_t) (owner * N + j)];
            if (pXX.empty()) pXX.assign ((size_t) nbins, 0.0);
            auto& sg = p.sub[(size_t) g];
            for (int k = 0; k < nbins; ++k)
            {
                const cplx c = X[(size_t) k] * std::conj (R[(size_t) k]);
                p.total[(size_t) k] += c;
                pXX[(size_t) k] += std::norm (X[(size_t) k]);
                const int kf = k + kLo;   // full-spectrum bin
                if (kf % kSubDecim == 0 && kf / kSubDecim < nfftSub / 2)
                    sg[(size_t) (kf / kSubDecim)] += std::complex<float> ((float) c.real(), (float) c.imag());
            }
            ++p.hits;
        }
    }

    // directed estimate: owner i's hits, j relative to i
    bool estimate (int i, int j, PairEdge& e)
    {
        const auto& p = pairs[(size_t) (i * N + j)];
        if (p.hits < 2) return false;
        const auto& pRR = power[(size_t) (i * N + i)];
        const auto& pXX = power[(size_t) (i * N + j)];
        // weights: half-whitened + A-weighted (as Phase 2)
        weights.assign ((size_t) nbins, cplx {});
        for (int k = 0; k < nbins; ++k)
        {
            const double m = std::sqrt (std::sqrt (pXX[(size_t) k] * pRR[(size_t) k]));
            if (m > 1e-30) weights[(size_t) k] = p.total[(size_t) k] / m * aw[(size_t) k];
        }
        const int L = std::min (maxLag, nfft / 2 - 1);
        correlate (weights, kLo, nfft, fft, work, L, coarse);
        int bestI = 0; double best = -1.0;
        for (int t = -L; t <= L; ++t) if (std::abs (coarse[(size_t) (t + L)]) > best) { best = std::abs (coarse[(size_t) (t + L)]); bestI = t; }
        if (best <= 0.0) return false;
        const double lag = refine ((double) bestI);
        const double peak = corrAt (lag);
        e.lag = lag;
        e.polarity = peak >= 0.0 ? 1 : -1;
        e.hits = p.hits;
        {
            const int guard = std::max (2, (int) std::lround (0.0003 * sr));
            double second = 0.0;
            for (int t = -L + 1; t < L; ++t)
            {
                const double a = std::abs (coarse[(size_t) (t + L)]);
                if (std::abs (t - bestI) > guard && a >= std::abs (coarse[(size_t) (t + L - 1)]) && a >= std::abs (coarse[(size_t) (t + L + 1)]))
                    second = std::max (second, a);
            }
            e.prominence = second > 0.0 ? best / second : 10.0;
        }

        // the 6 groups (coarse spectra): same lag (+-2 samples) and polarity?
        const int Ls = std::min (L, nfftSub / 2 - 1);
        int agree = 0;
        for (int g = 0; g < kSubsets; ++g)
        {
            subW.assign ((size_t) (nfftSub / 2 + 1), cplx {});
            double en = 0.0;
            for (size_t q = 0; q < p.sub[(size_t) g].size(); ++q)
            {
                const int kk = (int) q;                 // bin in the decimated spectrum
                const int k = kk * kSubDecim - kLo;     // index into the full (kLo-based) arrays
                if (k < 0 || k >= nbins || kk >= nfftSub / 2) continue;
                const double m = std::sqrt (std::sqrt (pXX[(size_t) k] * pRR[(size_t) k]));
                if (m <= 1e-30) continue;
                const auto c = p.sub[(size_t) g][q];
                subW[(size_t) kk] = cplx (c.real(), c.imag()) / m * aw[(size_t) k];
                en += std::abs (subW[(size_t) kk]);
            }
            if (en <= 0.0) continue;
            correlate (subW, 0, nfftSub, fftSub, workSub, Ls, sc, true);
            int bi = 0; double bv = -1.0;
            for (int t = -Ls; t <= Ls; ++t) if (std::abs (sc[(size_t) (t + Ls)]) > bv) { bv = std::abs (sc[(size_t) (t + Ls)]); bi = t; }
            const int pol = sc[(size_t) (bi + Ls)] >= 0.0 ? 1 : -1;
            if (std::abs ((double) bi - lag) <= 2.0 && pol == e.polarity) ++agree;
        }
        e.sure = p.hits >= 2 * kSubsets ? (double) agree / kSubsets : 0.0;
        return true;
    }

    // c[t] = Re sum W e^{+jwt}. W given from bin `first` upward (or full one-sided if fullSpectrum).
    static void correlate (const std::vector<cplx>& w, int first, int size, const Fft& f, std::vector<cplx>& wk,
                           int L, std::vector<double>& out, bool fullSpectrum = false)
    {
        std::fill (wk.begin(), wk.end(), cplx {});
        const int half = size / 2;
        for (size_t q = 0; q < w.size(); ++q)
        {
            const int k = fullSpectrum ? (int) q : (int) q + first;
            if (k <= 0 || k >= half) continue;
            wk[(size_t) k] = w[q];
            wk[(size_t) (size - k)] = std::conj (w[q]);
        }
        f.run (wk, true);
        out.assign ((size_t) (2 * L + 1), 0.0);
        for (int t = -L; t <= L; ++t) out[(size_t) (t + L)] = wk[(size_t) ((t + size) % size)].real();
    }

    double corrAt (double tau) const
    {
        double acc = 0.0;
        const double w0 = 2.0 * 3.14159265358979323846 / nfft;
        for (int q = 0; q < nbins; ++q)
        {
            const cplx& w = weights[(size_t) q];
            if (w == cplx {}) continue;
            const double ph = w0 * (q + kLo) * tau;
            acc += 2.0 * (w.real() * std::cos (ph) - w.imag() * std::sin (ph));
        }
        return acc;
    }

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
    int maxLag = 1440, N = 0, win = 8192, nfft = 16384, nfftSub = 4096, hist = 32768;
    int nb = 8193, kLo = 1, kHi = 6000, nbins = 6000;
    long long n = 0, refractory = 7200, preRoll = 1920;
    double holdDecay = 0.99, levelDecay = 0.9999;
    int nPending = 0;
    std::array<Pending, 256> pending {};
    Fft fft, fftSub;
    std::vector<double> hann, aw, coarse, sc;
    std::vector<Track> tracks;
    std::vector<Pair> pairs;
    std::vector<std::vector<double>> power;   // [owner*N + j]: |X_j|^2 over owner's hits ([i*N+i] = owner's own)
    std::vector<std::vector<cplx>> spec;
    std::vector<cplx> buf, work, workSub, weights, subW;
};

//------------------------------------------------------------------------------
// Turns pair measurements into one Delay/Polarity per track.
//
// A real kit doesn't have "one delay per mic": the overheads hear the hat, the toms and the snare from
// different spots, so a track's offset depends on whose sound you measure. Engineers line each close mic
// up with where ITS drum appears in the rest of the kit. So:
//  1. Groups = tracks connected by confident pairs (sure >= 0.66). Each group grows from a root: the REF
//     track, or the most-connected track (usually an overhead or the snare).
//  2. Repeatedly, the unplaced track with the best-supported position joins. Its candidate positions
//     come from every confident pair with an already-placed track, preferring the measurement made on
//     the joining track's OWN hits (its sound as heard by the others; the other direction counts half).
//     Candidates within 3 samples and with the same polarity vote together.
//  3. A track stays only if >= 40 % of its link evidence (against every other placed track) agrees. Otherwise it's left alone (kick-in mics,
//     rarely-played toms...) rather than placed on a split vote.
//  4. Without a REF the group is shifted so its earliest track sits at Delay 0 (everything else moves
//     earlier, like Phase 2). With a REF, that track stays at 0.
// Measured on Corey's drum session (Songs 1/3/4): the same answer song after song for overheads, rooms,
// toms, snare bottom (0.45 ms, inverted), LDC and ride; the ambiguous tracks were the low-support ones.
// What the user and the track names say (both optional; empty = all Auto, names unknown).
//   group[i]:  -1 = Auto, -2 = Off (sits out: buses, effects), 0..7 = a manual group (A..H)
//   family[i]: from the track name (AlignNames.h)
// Manual groups are closed: a track in group A only links with other A tracks (and vice versa), and
// inside a manual group the bar is lower (sure >= 0.5, support >= 25 %): the user has said these belong
// together, so a weaker but agreeing measurement is used instead of leaving the track alone.
// Auto tracks link only with Auto tracks, never across two different known families.
struct SessionHints
{
    std::vector<int> group;
    std::vector<Family> family;
    int groupOf (int i) const     { return i >= 0 && i < (int) group.size() ? group[(size_t) i] : -1; }
    Family familyOf (int i) const { return i >= 0 && i < (int) family.size() ? family[(size_t) i] : Family::unknown; }
    bool off (int i) const        { return groupOf (i) == -2; }
    bool manual (int i) const     { return groupOf (i) >= 0; }
    bool mayLink (int a, int b) const
    {
        if (off (a) || off (b)) return false;
        const int ga = groupOf (a), gb = groupOf (b);
        if (ga >= 0 || gb >= 0) return ga == gb;
        return familiesMayLink (familyOf (a), familyOf (b));
    }
};

inline double edgeWeight (const PairEdge& e)
{
    const double prom = std::clamp ((e.prominence - 1.0) / 0.3, 0.15, 1.0);
    return e.sure * e.sure * std::min (1.0, e.hits / 60.0) * prom + 1e-9;
}

// directed: e.a = whose hits were used, e.b relative to e.a (lag = t_b - t_a)
inline std::vector<TrackAlign> solveSession (int N, const std::vector<PairEdge>& directed, int refTrack, double maxShiftSamples,
                                             const SessionHints& hints = {})
{
    constexpr double minSure = 0.66, minSureManual = 0.5, tol = 3.0, minSupportAuto = 0.4, minSupportManual = 0.25;
    std::vector<TrackAlign> res ((size_t) N);
    for (int i = 0; i < N; ++i) if (hints.off (i)) res[(size_t) i].why = LeftAlone::off;
    if (refTrack >= 0 && (refTrack >= N || hints.off (refTrack))) refTrack = -1;
    std::vector<const PairEdge*> E;
    for (auto& e : directed)
    {
        if (e.a == e.b || e.a < 0 || e.b < 0 || e.a >= N || e.b >= N || ! hints.mayLink (e.a, e.b)) continue;
        const bool inManual = hints.manual (e.a);   // mayLink: both in the same manual group
        if (e.sure >= (inManual ? minSureManual : minSure)) E.push_back (&e);
    }

    // connected components on confident pairs
    std::vector<int> comp ((size_t) N, -1);
    int nComp = 0;
    for (int s = 0; s < N; ++s)
    {
        if (comp[(size_t) s] >= 0) continue;
        std::vector<int> stack { s }; comp[(size_t) s] = nComp;
        while (! stack.empty())
        {
            const int v = stack.back(); stack.pop_back();
            for (auto* e : E)
            {
                const int o = e->a == v ? e->b : (e->b == v ? e->a : -1);
                if (o >= 0 && comp[(size_t) o] < 0) { comp[(size_t) o] = nComp; stack.push_back (o); }
            }
        }
        ++nComp;
    }

    // a manual group that falls apart into pieces: keep the piece with the REF, else the biggest
    for (int g = 0; g < 8; ++g)
    {
        std::vector<int> size ((size_t) nComp, 0);
        int keep = -1;
        for (int v = 0; v < N; ++v) if (hints.groupOf (v) == g) ++size[(size_t) comp[(size_t) v]];
        for (int c = 0; c < nComp; ++c) if (size[(size_t) c] > 0 && (keep < 0 || size[(size_t) c] > size[(size_t) keep])) keep = c;
        if (refTrack >= 0 && hints.groupOf (refTrack) == g) keep = comp[(size_t) refTrack];
        for (int v = 0; v < N; ++v)
            if (hints.groupOf (v) == g && comp[(size_t) v] != keep) { res[(size_t) v].why = LeftAlone::notWithGroup; comp[(size_t) v] = -1; }
    }

    unsigned usedLetters = 0;
    for (int v = 0; v < N; ++v) if (hints.manual (v)) usedLetters |= 1u << hints.groupOf (v);

    int groupId = 0;
    for (int c = 0; c < nComp; ++c)
    {
        std::vector<int> members;
        for (int v = 0; v < N; ++v) if (comp[(size_t) v] == c) members.push_back (v);
        if (members.size() < 2) continue;
        auto minSupportOf = [&] (int v) { return hints.manual (v) ? minSupportManual : minSupportAuto; };

        int root = -1;
        if (refTrack >= 0 && comp[(size_t) refTrack] == c) root = refTrack;
        else
        {
            double bestScore = -1.0;
            for (int v : members)
            {
                double sc = 0.0;
                for (auto* e : E) if (e->a == v || e->b == v) sc += edgeWeight (*e);
                if (sc > bestScore) { bestScore = sc; root = v; }
            }
        }

        std::vector<char> excluded ((size_t) N, 0);
        std::vector<char> placed ((size_t) N, 0);
        std::vector<double> t ((size_t) N, 0.0), sure ((size_t) N, 0.0);
        std::vector<int> pol ((size_t) N, 1);
        std::vector<int> order;

        struct Imp { double t; int p; double w, sure; };
        // candidate positions for track j from every confident pair with a placed track
        auto implied = [&] (int j, std::vector<Imp>& imp)
        {
            imp.clear();
            for (auto* e : E)
            {
                if (e->a == j && e->b != j && placed[(size_t) e->b])       // j's own hits: t_b - t_j = lag
                    imp.push_back ({ t[(size_t) e->b] - e->lag, pol[(size_t) e->b] * e->polarity, edgeWeight (*e), e->sure });
                else if (e->b == j && e->a != j && placed[(size_t) e->a])  // the placed track's hits (counts half)
                    imp.push_back ({ t[(size_t) e->a] + e->lag, pol[(size_t) e->a] * e->polarity, 0.5 * edgeWeight (*e), e->sure });
            }
        };
        // best-supported position: (support weight, total weight, position, polarity, max sure among supporters)
        auto vote = [&] (const std::vector<Imp>& imp, double& sw, double& tot, double& pos, int& p, double& ms)
        {
            sw = tot = 0.0; ms = 0.0;
            for (auto& x : imp) tot += x.w;
            for (auto& c : imp)
            {
                double w = 0.0, st = 0.0, m = 0.0;
                for (auto& x : imp) if (x.p == c.p && std::abs (x.t - c.t) <= tol) { w += x.w; st += x.w * x.t; m = std::max (m, x.sure); }
                if (w > sw) { sw = w; pos = st / w; p = c.p; ms = m; }
            }
        };

        for (int round = 0; round < N; ++round)
        {
            std::fill (placed.begin(), placed.end(), 0);
            order.assign (1, root);
            placed[(size_t) root] = 1; t[(size_t) root] = 0.0; pol[(size_t) root] = 1; sure[(size_t) root] = 1.0;
            std::vector<Imp> imp;
            for (;;)   // grow: the best-supported unplaced track joins
            {
                int bestJ = -1; double bestW = 0.0, bestT = 0.0, bestS = 0.0; int bestP = 1;
                for (int j : members)
                {
                    if (placed[(size_t) j] || excluded[(size_t) j]) continue;
                    implied (j, imp);
                    if (imp.empty()) continue;
                    double sw, tot, pos = 0.0, ms; int p = 1;
                    vote (imp, sw, tot, pos, p, ms);
                    if (sw > bestW) { bestW = sw; bestJ = j; bestT = pos; bestP = p; bestS = ms * sw / tot; }
                }
                if (bestJ < 0) break;
                placed[(size_t) bestJ] = 1; t[(size_t) bestJ] = bestT; pol[(size_t) bestJ] = bestP; sure[(size_t) bestJ] = bestS;
                order.push_back (bestJ);
            }
            // judge every placed track against ALL other placed tracks; set aside the least supported one
            int worst = -1; double worstMargin = 0.0;
            for (int j : order)
            {
                if (j == root) continue;
                placed[(size_t) j] = 0;   // look at it from the others' point of view
                implied (j, imp);
                placed[(size_t) j] = 1;
                if (imp.empty()) continue;
                double sw, tot, pos = 0.0, ms; int p = 1;
                vote (imp, sw, tot, pos, p, ms);
                const bool same = p == pol[(size_t) j] && std::abs (pos - t[(size_t) j]) <= tol;
                const double frac = same ? sw / tot : 0.0;
                sure[(size_t) j] = same ? ms * frac : 0.0;
                const double margin = frac - minSupportOf (j);   // below 0 = not supported enough
                if (margin < worstMargin) { worstMargin = margin; worst = j; }
            }
            if (worst < 0) break;
            excluded[(size_t) worst] = 1;
        }
        for (int v : members) res[(size_t) v].why = LeftAlone::disagree;   // until placed below
        if (order.size() < 2) continue;

        int letter = -1;
        for (int v : order) if (hints.manual (v)) letter = hints.groupOf (v);
        if (letter < 0) { letter = 0; while (letter < 25 && (usedLetters & (1u << letter))) ++letter; }
        usedLetters |= 1u << letter;

        // anchor: REF, or the earliest placed track
        int anchor = root;
        if (! (refTrack >= 0 && refTrack == root))
            for (int v : order) if (t[(size_t) v] < t[(size_t) anchor]) anchor = v;
        for (int v : order)
        {
            auto& r = res[(size_t) v];
            r.group = groupId;
            r.letter = letter;
            r.why = LeftAlone::no;
            double d = -(t[(size_t) v] - t[(size_t) anchor]);
            if (std::abs (d) > maxShiftSamples) { r.clamped = true; d = std::clamp (d, -maxShiftSamples, maxShiftSamples); }
            r.delaySamples = d;
            r.polarity = pol[(size_t) v] * pol[(size_t) anchor];
            r.sure = v == anchor ? 1.0 : sure[(size_t) v];
            r.anchor = v == anchor;
        }
        ++groupId;
    }
    return res;
}

} // namespace sa
