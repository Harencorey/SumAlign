//==============================================================================
// SumAlign - whole-session ALIGN tests (no JUCE): a synthetic multi-mic kit plus an unrelated track.
//
// Four sources (kick, snare, tom, hat) are heard by seven mics. Every mic hears every source
// (with its own level and darkening), arriving at  source time + that mic's own delay, so there is
// one right answer per mic. The snare bottom is inverted. A vocal-like track shares nothing.
//==============================================================================

#include "AlignNetwork.h"
#include "AlignHub.h"
#include <thread>
#include "AlignCore.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int gFailures = 0;
#define CHECK(cond, ...) \
    do { if (! (cond)) { ++gFailures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

using Vec = std::vector<float>;
constexpr double kPi = 3.14159265358979323846;

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
        a[(size_t) k] *= std::polar (1.0, k == n / 2 ? 0.0 : -2.0 * kPi * kk * tau / n);
    }
    f.run (a, true);
    Vec y (x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = (float) (a[i].real() / n);
    return y;
}

// one-pole darkening applied forwards and backwards (zero phase: no timing change)
static Vec darker (const Vec& x, double hz, double sr)
{
    const double a = std::exp (-2 * kPi * hz / sr);
    Vec y = x;
    double s = 0; for (auto& v : y) { s = (1 - a) * v + a * s; v = (float) s; }
    s = 0; for (size_t i = y.size(); i-- > 0;) { s = (1 - a) * y[i] + a * s; y[i] = (float) s; }
    return y;
}

static Vec hits (size_t n, double sr, unsigned seed, double bodyHz, double rate, double decay, double noiseAmt)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> u (-1, 1), gap (0.5, 1.5), amp (0.4, 1.0);
    Vec y (n, 0.0f);
    for (double t = 0.1 * sr; t < (double) n; t += sr / rate * gap (rng))
    {
        const size_t at = (size_t) t; const double a = amp (rng);
        for (size_t i = 0; i < (size_t) (0.5 * sr) && at + i < n; ++i)
        {
            const double tt = i / sr;
            const double body = bodyHz > 0 ? std::exp (-tt / decay) * std::sin (2 * kPi * bodyHz * (1 + 0.6 * std::exp (-tt / 0.008)) * tt) : 0.0;
            y[at + i] += (float) (a * (body + noiseAmt * std::exp (-tt / (bodyHz > 0 ? 0.005 : decay)) * u (rng)));
        }
    }
    return y;
}

struct Kit { std::vector<Vec> tracks; std::vector<std::string> names; std::vector<double> delay; std::vector<int> pol; };

static Kit makeKit (double sr, double seconds, unsigned seed)
{
    const size_t n = (size_t) (seconds * sr);
    const double k = sr / 48000.0;
    std::vector<Vec> src {
        hits (n, sr, seed + 1, 55.0, 2.0, 0.15, 0.4),   // kick
        hits (n, sr, seed + 2, 200.0, 1.7, 0.12, 0.9),  // snare
        hits (n, sr, seed + 3, 110.0, 0.8, 0.25, 0.5),  // tom
        hits (n, sr, seed + 4, 0.0, 4.0, 0.04, 0.5) };  // hat (noise)
    Kit kit;
    kit.names = { "Kick In", "Snare Top", "Snare Bot", "Tom", "OH L", "OH R", "Room", "Vocal" };
    kit.delay = { 0.0, 5.3 * k, 25.7 * k, 12.2 * k, 160.4 * k, 175.9 * k, 765.1 * k, 0.0 };
    kit.pol   = { 1, 1, -1, 1, 1, 1, 1, 1 };
    // level of each source in each mic  [mic][source]           kick  snare  tom  hat
    const double g[7][4] = { { 1.0, 0.15, 0.10, 0.05 },   // kick in
                             { 0.20, 1.0, 0.25, 0.30 },   // snare top
                             { 0.15, 0.8, 0.15, 0.10 },   // snare bot
                             { 0.15, 0.30, 1.0, 0.15 },   // tom
                             { 0.30, 0.6, 0.5, 0.8 },     // OH L
                             { 0.30, 0.6, 0.4, 0.6 },     // OH R
                             { 0.5, 0.5, 0.5, 0.4 } };    // room
    const double dark[7] = { 6000, 12000, 9000, 10000, 9000, 9000, 4000 };
    std::mt19937 rng (seed + 99);
    for (int m = 0; m < 7; ++m)
    {
        Vec y (n, 0.0f);
        for (int s = 0; s < 4; ++s)
        {
            // each source sits at a slightly different spot: +-0.5 samples of extra path (as on a real kit)
            const double jitter = (s - 1.5) * 0.3 * (m >= 4 ? 1.0 : 0.0) * k;
            Vec d = darker (delayExact (src[(size_t) s], kit.delay[(size_t) m] + jitter), dark[m], sr);
            for (size_t i = 0; i < n; ++i) y[i] += (float) (kit.pol[(size_t) m] * g[m][s] * d[i]);
        }
        if (m == 6) { Vec late = darker (delayExact (y, 0.012 * sr), 2500, sr); for (size_t i = 0; i < n; ++i) y[i] += 0.4f * late[i]; }   // room reverb-ish
        std::normal_distribution<double> nz (0, 0.0005);
        for (auto& v : y) v += (float) nz (rng);
        kit.tracks.push_back (y);
    }
    // vocal: phrases of a gliding harmonic tone + breath noise, unrelated to the kit
    Vec v (n, 0.0f);
    std::uniform_real_distribution<double> u (-1, 1);
    double ph = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double on = std::sin (2 * kPi * 0.31 * t) > 0 ? 1.0 : 0.0;
        const double f0 = 220 + 40 * std::sin (2 * kPi * 0.7 * t);
        ph += 2 * kPi * f0 / sr;
        v[i] = (float) (on * (0.3 * std::sin (ph) + 0.15 * std::sin (2 * ph) + 0.02 * u (rng)));
    }
    kit.tracks.push_back (v);
    return kit;
}

static std::vector<sa::TrackAlign> runKit (const Kit& kit, double sr, int ref, std::vector<sa::PairEdge>* edgesOut = nullptr)
{
    const int N = (int) kit.tracks.size();
    const int maxLag = (int) std::lround (sa::kMaxShiftMs * 0.001 * sr);
    sa::NetworkEstimator est;
    est.prepare (sr, maxLag, N);
    std::vector<const float*> p ((size_t) N);
    const size_t n = kit.tracks[0].size();
    for (size_t pos = 0; pos + 512 <= n; pos += 512)
    {
        for (int i = 0; i < N; ++i) p[(size_t) i] = kit.tracks[(size_t) i].data() + pos;
        est.feed (p.data(), 512);
    }
    auto e = est.directedEdges();
    if (edgesOut) *edgesOut = e;
    return sa::solveSession (N, e, ref, maxLag);
}

static void testKit()
{
    std::printf ("Synthetic kit: one group, every mic's offset from the earliest mic, polarity, vocal left alone\n");
    for (double sr : { 48000.0, 96000.0 })
    {
        auto kit = makeKit (sr, 25.0, 7);
        std::vector<sa::PairEdge> edges;
        auto r = runKit (kit, sr, -1, &edges);
        for (size_t m = 0; m < 7; ++m)
        {
            const double want = -(kit.delay[m] - kit.delay[0]);
            CHECK (r[m].group == 0, "%s: group %d", kit.names[m].c_str(), r[m].group);
            CHECK (std::abs (r[m].delaySamples - want) < 0.6, "%s at %.0f: delay %.2f, want %.2f", kit.names[m].c_str(), sr, r[m].delaySamples, want);
            CHECK (r[m].polarity == kit.pol[m], "%s: polarity %d", kit.names[m].c_str(), r[m].polarity);
            if (sr == 48000.0)
                std::printf ("  %-10s group %d  delay %+9.2f (want %+9.2f)  pol %+d  sure %.2f%s\n", kit.names[m].c_str(), r[m].group,
                             r[m].delaySamples, want, r[m].polarity, r[m].sure, r[m].anchor ? "  [anchor]" : "");
        }
        CHECK (r[7].group == -1, "vocal grouped (%d)", r[7].group);
        CHECK (r[0].anchor, "the earliest mic (kick in) isn't the anchor");
        if (sr == 48000.0)
        {
            int conf = 0; for (auto& e : edges) if (e.sure >= 0.66) ++conf;
            std::printf ("  Vocal      group %d (ungrouped = left alone)   %zu pairs measured, %d confident\n", r[7].group, edges.size(), conf);
        }
    }
}

static void testRef()
{
    std::printf ("REF moves the anchor: everything relative to the snare top instead\n");
    const double sr = 48000.0;
    auto kit = makeKit (sr, 25.0, 11);
    auto r = runKit (kit, sr, 1);
    for (size_t m = 0; m < 7; ++m)
    {
        const double want = -(kit.delay[m] - kit.delay[1]);
        CHECK (std::abs (r[m].delaySamples - want) < 0.6, "%s: %.2f want %.2f", kit.names[m].c_str(), r[m].delaySamples, want);
    }
    CHECK (r[1].anchor && std::abs (r[1].delaySamples) < 1e-9, "snare top not the anchor");
    CHECK (r[0].delaySamples > 4.0, "kick in should move later (+5.3), got %.2f", r[0].delaySamples);
}

static void testSolverRobust()
{
    std::printf ("Solver: outvotes a confident-but-wrong pair, leaves a split-vote track alone, splits groups, clamps\n");
    // 5 tracks, true arrival times
    const double t[5] = { 0, 10.5, 30.25, 160, 400 };
    std::vector<sa::PairEdge> e;
    for (int a = 0; a < 5; ++a) for (int b = a + 1; b < 5; ++b) e.push_back ({ a, b, t[b] - t[a], 1, 0.9, 80, 1.5 });
    e.push_back ({ 0, 0, 0, 1, 0.0, 0, 1.0 });   // junk below minSure, ignored
    e[3].lag += 37.0;                       // 0-4 wrong by 37 samples (an alternate peak)
    auto both = [] (std::vector<sa::PairEdge> v)
    {
        std::vector<sa::PairEdge> out;
        for (auto x : v) { out.push_back (x); std::swap (x.a, x.b); x.lag = -x.lag; out.push_back (x); }
        return out;
    };
    auto r = sa::solveSession (5, both (e), -1, 1440);
    for (int m = 0; m < 5; ++m) CHECK (std::abs (r[(size_t) m].delaySamples + t[m]) < 1e-6, "track %d delay %.3f want %.3f", m, r[(size_t) m].delaySamples, -t[m]);

    // a track whose two strong links disagree joins neither side's timing on a split vote
    // (its three links point three different ways: no position gets 40 % of the vote)
    std::vector<sa::PairEdge> c { { 0, 1, 10.0, 1, 1.0, 80, 1.5 }, { 0, 2, 20.0, 1, 1.0, 80, 1.5 }, { 1, 2, 10.0, 1, 1.0, 80, 1.5 },
                                  { 3, 0, 50.0, 1, 1.0, 80, 1.3 }, { 3, 1, 140.0, -1, 1.0, 80, 1.3 }, { 3, 2, -77.0, 1, 1.0, 80, 1.3 } };
    r = sa::solveSession (4, both (c), -1, 1440);
    CHECK (r[0].group == r[1].group && r[1].group == r[2].group && r[0].group >= 0 && r[3].group != r[0].group,
           "split-vote track 3 joined the group (%d vs %d)", r[3].group, r[0].group);

    // two separate groups + a loner
    std::vector<sa::PairEdge> g { { 0, 1, 5.0, -1, 0.9, 50, 1.5 }, { 2, 3, -8.0, 1, 0.85, 50, 1.5 } };
    r = sa::solveSession (5, both (g), -1, 1440);
    CHECK (r[0].group >= 0 && r[0].group == r[1].group && r[2].group >= 0 && r[2].group == r[3].group && r[0].group != r[2].group && r[4].group == -1,
           "groups %d %d %d %d %d", r[0].group, r[1].group, r[2].group, r[3].group, r[4].group);
    CHECK (r[1].polarity == -1 && r[0].polarity == 1, "polarity in group 0");
    CHECK (r[3].anchor && std::abs (r[2].delaySamples + 8.0) < 1e-9, "group 1: track 3 is earliest (anchor), track 2 should get -8 (got %.2f)", r[2].delaySamples);

    // beyond the range
    std::vector<sa::PairEdge> far { { 0, 1, 2000.0, 1, 0.9, 50, 1.5 } };
    r = sa::solveSession (2, both (far), -1, 1440);
    CHECK (r[1].clamped && std::abs (r[1].delaySamples + 1440) < 1e-9, "clamp: %.1f", r[1].delaySamples);

    // the split-vote track is reported as "measurements disagree", the loner as "nothing shared"
    r = sa::solveSession (4, both (c), -1, 1440);
    CHECK (r[3].why == sa::LeftAlone::disagree, "split-vote reason %d", (int) r[3].why);
    r = sa::solveSession (5, both (g), -1, 1440);
    CHECK (r[4].why == sa::LeftAlone::nothingShared && r[0].why == sa::LeftAlone::no, "loner reason %d", (int) r[4].why);
}

static std::vector<sa::PairEdge> bothWays (std::vector<sa::PairEdge> v)
{
    std::vector<sa::PairEdge> out;
    for (auto x : v) { out.push_back (x); std::swap (x.a, x.b); x.lag = -x.lag; out.push_back (x); }
    return out;
}

static void testNames()
{
    std::printf ("Track names: family and bus\n");
    using F = sa::Family;
    struct C { const char* name; F fam; bool bus; };
    const C cases[] = {
        { "Kick trig", F::drums, false }, { "Kick In", F::drums, false }, { "KICK", F::drums, false }, { "Kick Port", F::drums, false },
        { "SNARE", F::drums, false }, { "Snare Top LDC", F::drums, false }, { "Snare Bot", F::drums, false }, { "SnareBottom", F::drums, false },
        { "Sn Btm", F::drums, false }, { "FT2", F::drums, false }, { "Fl1", F::drums, false }, { "Floor Tom", F::drums, false },
        { "RT1", F::drums, false }, { "Rack Tom 2", F::drums, false }, { "OH L", F::drums, false }, { "Overheads", F::drums, false },
        { "Room R", F::drums, false }, { "Hi-Hat", F::drums, false }, { "HH", F::drums, false }, { "Ride", F::drums, false },
        { "Bass Drum", F::drums, false }, { "BassDrum", F::drums, false },
        { "Bass DI", F::bass, false }, { "Bass Amp", F::bass, false }, { "BASS_D.I.", F::bass, false }, { "Bs Mic", F::bass, false },
        { "Gtr Room", F::guitar, false }, { "Guitar DI", F::guitar, false }, { "Gtr 414", F::guitar, false }, { "Ac Gtr", F::guitar, false },
        { "Lead Vox", F::vocals, false }, { "BGV 2", F::vocals, false }, { "Piano L", F::keys, false },
        { "414", F::unknown, false }, { "Ribbon", F::unknown, false }, { "Audio 3", F::unknown, false }, { "LDC", F::unknown, false }, { "DI", F::unknown, false },
        { "Drum Bus", F::drums, true }, { "Kick Aux", F::drums, true }, { "Verb", F::unknown, true }, { "Drum Smash", F::drums, true },
        { "Bass Bus", F::bass, true }, { "Gtr Stem", F::guitar, true }, { "FX Return", F::unknown, true },
    };
    int bad = 0;
    for (auto& c : cases)
    {
        const auto ni = sa::classifyTrackName (c.name);
        if (ni.family != c.fam || ni.bus != c.bus)
        {
            ++bad;
            CHECK (false, "'%s': family '%s' bus %d, want '%s' bus %d", c.name, sa::familyName (ni.family), (int) ni.bus, sa::familyName (c.fam), (int) c.bus);
        }
    }
    std::printf ("  %d names, %d wrong\n", (int) (sizeof (cases) / sizeof (cases[0])), bad);
    CHECK (! sa::familiesMayLink (F::drums, F::bass) && sa::familiesMayLink (F::bass, F::bass) && sa::familiesMayLink (F::unknown, F::guitar), "familiesMayLink");
}

static void testHints()
{
    std::printf ("Hints: families keep bleed out, manual groups, Off\n");
    using F = sa::Family;
    // 0 kick in, 1 snare, 2 OH  (drums)   3 bass DI, 4 bass amp (bass)
    // the bass amp mic hears the kick confidently (bleed), and the DI/amp pair links the two bass tracks
    std::vector<sa::PairEdge> e { { 0, 1, 20.0, 1, 0.9, 80, 1.5 }, { 0, 2, 150.0, 1, 0.9, 80, 1.5 }, { 1, 2, 130.0, 1, 0.9, 80, 1.5 },
                                  { 0, 4, 400.0, 1, 0.9, 80, 1.5 }, { 3, 4, 60.0, -1, 0.9, 80, 1.5 } };
    const auto E = bothWays (e);

    auto r = sa::solveSession (5, E, -1, 1440);
    CHECK (r[0].group >= 0 && r[0].group == r[4].group && r[3].group == r[4].group, "without names the bleed joins everything (%d %d %d)", r[0].group, r[3].group, r[4].group);

    sa::SessionHints h;
    h.family = { F::drums, F::drums, F::drums, F::bass, F::bass };
    r = sa::solveSession (5, E, -1, 1440, h);
    CHECK (r[0].group >= 0 && r[0].group == r[1].group && r[1].group == r[2].group, "drums together");
    CHECK (r[3].group >= 0 && r[3].group == r[4].group && r[3].group != r[0].group, "bass DI + amp their own group (%d %d vs %d)", r[3].group, r[4].group, r[0].group);
    CHECK (r[3].anchor && std::abs (r[4].delaySamples + 60.0) < 1e-9 && r[4].polarity == -1, "bass amp: %.2f pol %d", r[4].delaySamples, r[4].polarity);
    CHECK (r[0].letter == 0 && r[3].letter == 1, "letters %d %d", r[0].letter, r[3].letter);

    // manual: a weak (0.55) link that Auto ignores is used when both are put in group C; auto group takes A
    std::vector<sa::PairEdge> w { { 0, 1, 12.0, 1, 0.9, 80, 1.5 }, { 2, 3, -7.0, 1, 0.55, 80, 1.5 } };
    r = sa::solveSession (4, bothWays (w), -1, 1440);
    CHECK (r[2].group == -1 && r[3].group == -1, "weak link grouped in Auto");
    sa::SessionHints m; m.group = { -1, -1, 2, 2 };
    r = sa::solveSession (4, bothWays (w), -1, 1440, m);
    CHECK (r[2].group >= 0 && r[2].group == r[3].group && r[2].letter == 2 && std::abs (r[2].delaySamples + 7.0) < 1e-9 && std::abs (r[3].delaySamples) < 1e-9
           && r[3].anchor, "manual C: %d %d letter %d, %.2f %.2f", r[2].group, r[3].group, r[2].letter, r[2].delaySamples, r[3].delaySamples);
    CHECK (r[0].letter == 0, "auto group letter %d (want A)", r[0].letter);

    // manual groups are closed: a track set to A doesn't link to an Auto track, even with a strong link
    sa::SessionHints c; c.group = { 0, 0, -1, -1 };
    std::vector<sa::PairEdge> x { { 0, 1, 12.0, 1, 0.9, 80, 1.5 }, { 1, 2, 30.0, 1, 0.95, 80, 1.5 }, { 2, 3, 5.0, 1, 0.9, 80, 1.5 } };
    r = sa::solveSession (4, bothWays (x), -1, 1440, c);
    CHECK (r[0].group == r[1].group && r[2].group == r[3].group && r[0].group != r[2].group && r[0].group >= 0 && r[2].group >= 0, "closed groups");
    CHECK (r[0].letter == 0 && r[2].letter == 1, "letters %d %d", r[0].letter, r[2].letter);

    // a manual-group member that shares nothing with its group
    sa::SessionHints s; s.group = { 0, 0, 0 };
    r = sa::solveSession (3, bothWays ({ { 0, 1, 12.0, 1, 0.9, 80, 1.5 } }), -1, 1440, s);
    CHECK (r[2].group == -1 && r[2].why == sa::LeftAlone::notWithGroup, "lone A member: group %d why %d", r[2].group, (int) r[2].why);

    // Off: sits out, and a REF on it is ignored
    sa::SessionHints o; o.group = { -1, -1, -2 };
    r = sa::solveSession (3, bothWays ({ { 0, 1, 12.0, 1, 0.9, 80, 1.5 }, { 0, 2, 3.0, 1, 0.99, 80, 1.5 }, { 1, 2, -9.0, 1, 0.99, 80, 1.5 } }), 2, 1440, o);
    CHECK (r[2].group == -1 && r[2].why == sa::LeftAlone::off && r[0].group >= 0 && r[0].anchor, "off: %d why %d", r[2].group, (int) r[2].why);
}

static void testHub()
{
    std::printf ("Hub: 8 instances push their tracks on the DAW timeline (with a loop jump); the hub lines them up and solves\n");
    const double sr = 48000.0;
    auto kit = makeKit (sr, 40.0, 21);
    auto hub = sa::AlignHub::get();
    std::vector<int> ids;
    for (auto& n : kit.names) ids.push_back (hub->join (n));
    CHECK (hub->numMembers() == 8, "members %d", hub->numMembers());
    const int maxLag = (int) std::lround (sa::kMaxShiftMs * 0.001 * sr);
    hub->start (sr, maxLag);
    const int block = 512;
    const size_t n = kit.tracks[0].size();
    long long timeline = 960000;   // the song starts 20 s into the timeline
    size_t pos = 0;
    bool jumped = false;
    while (pos + block <= n && hub->isListening())
    {
        // each "instance" pushes its own block (order varies, as threads would)
        for (size_t k = 0; k < ids.size(); ++k)
        {
            const size_t m = (k * 3 + pos / block) % ids.size();
            hub->push (ids[m], timeline, kit.tracks[m].data() + pos, block, sr);
        }
        pos += block; timeline += block;
        if (! jumped && pos > n / 2) { pos -= (size_t) (8 * sr); timeline -= (long long) (8 * sr); jumped = true; }   // loop back 8 s
        std::this_thread::sleep_for (std::chrono::microseconds (3500));   // ~3x real time
    }
    for (int i = 0; i < 400 && hub->isListening(); ++i) std::this_thread::sleep_for (std::chrono::milliseconds (10));
    const bool finishedItself = ! hub->isListening();
    if (hub->isListening()) { hub->finishNow(); std::this_thread::sleep_for (std::chrono::milliseconds (500)); }
    const auto st = hub->getStatus();
    CHECK (st.state == sa::AlignHub::State::done, "hub state %d", (int) st.state);
    for (size_t m = 0; m < st.members.size(); ++m)
    {
        const auto& r = st.members[m].result;
        if (m < 7)
        {
            const double want = -(kit.delay[m] - kit.delay[0]);
            CHECK (r.group == 0 && std::abs (r.delaySamples - want) < 0.6 && r.polarity == kit.pol[m],
                   "%s: group %d delay %.2f (want %.2f) pol %d", kit.names[m].c_str(), r.group, r.delaySamples, want, r.polarity);
        }
        else CHECK (r.group == -1, "vocal grouped");
    }
    std::printf ("  finished after %.1f s of audio, %d group(s)%s\n", st.heardSeconds, st.groups, finishedItself ? " (by itself)" : " (FINISH)");
    for (int id : ids) hub->leave (id);
    CHECK (hub->numMembers() == 0, "members left %d", hub->numMembers());
}

static void testHubHints()
{
    std::printf ("Hub: a 'Drum Bus' instance (the sum of the kit) sits out; a track set to Off sits out\n");
    const double sr = 48000.0;
    auto kit = makeKit (sr, 25.0, 31);
    auto hub = sa::AlignHub::get();
    std::vector<int> ids;
    for (auto& n : kit.names) ids.push_back (hub->join (n));
    const int busId = hub->join ("Drum Bus");
    hub->setGroup (ids[7], -2);   // the vocal: Off
    Vec bus (kit.tracks[0].size(), 0.0f);
    for (size_t m = 0; m < 7; ++m) for (size_t i = 0; i < bus.size(); ++i) bus[i] += kit.tracks[m][i];
    hub->start (sr, (int) std::lround (sa::kMaxShiftMs * 0.001 * sr));
    long long timeline = 0;
    for (size_t pos = 0; pos + 512 <= bus.size(); pos += 512, timeline += 512)
    {
        for (size_t m = 0; m < ids.size(); ++m) hub->push (ids[m], timeline, kit.tracks[m].data() + pos, 512, sr);
        hub->push (busId, timeline, bus.data() + pos, 512, sr);
        std::this_thread::sleep_for (std::chrono::microseconds (3500));   // ~3x real time, like the DAW
    }
    for (int i = 0; i < 400 && hub->isListening(); ++i) std::this_thread::sleep_for (std::chrono::milliseconds (10));
    if (hub->isListening()) { hub->finishNow(); std::this_thread::sleep_for (std::chrono::milliseconds (500)); }
    const auto st = hub->getStatus();
    CHECK (st.state == sa::AlignHub::State::done, "state %d", (int) st.state);
    for (auto& mv : st.members)
    {
        if (mv.id == busId) CHECK (mv.result.group == -1 && mv.result.why == sa::LeftAlone::off && mv.off(), "bus: group %d why %d", mv.result.group, (int) mv.result.why);
        else if (mv.id == ids[7]) CHECK (mv.result.why == sa::LeftAlone::off, "vocal set to Off: why %d", (int) mv.result.why);
        else
        {
            size_t m = 0; while (ids[m] != mv.id) ++m;
            const double want = -(kit.delay[m] - kit.delay[0]);
            CHECK (mv.result.group >= 0 && std::abs (mv.result.delaySamples - want) < 0.6, "%s: %.2f want %.2f", kit.names[m].c_str(), mv.result.delaySamples, want);
            CHECK (mv.family == sa::Family::drums || kit.names[m].find ("LDC") != std::string::npos, "%s family '%s'", kit.names[m].c_str(), sa::familyName (mv.family));
        }
    }
    for (int id : ids) hub->leave (id);
    hub->leave (busId);
}

int main()
{
    testKit();
    testRef();
    testSolverRobust();
    testNames();
    testHints();
    testHub();
    testHubHints();
    if (gFailures == 0) std::printf ("\nALL NETWORK TESTS PASSED\n");
    else                std::printf ("\n%d FAILURE(S)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
