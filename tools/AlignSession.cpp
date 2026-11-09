//==============================================================================
// AlignSession - whole-session ALIGN on a folder of WAV files (every track at once), as Phase 3 does.
//
//   AlignSession <folder> [REF name part] [seconds]
//
// Prints each track's group, Delay, Polarity and "sure", the anchor, and how many pairs were confident.
// Plain C++17:  g++ -O2 -std=c++17 -pthread -ISource tools/AlignSession.cpp -o AlignSession
//==============================================================================

#include "AlignNetwork.h"
#include "AlignCore.h"
#include "WavIO.h"

#include <chrono>
#include <dirent.h>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: AlignSession folder [REF name part] [seconds]\n"); return 2; }
    const std::string dir = argv[1];
    const std::string refPart = argc > 2 ? argv[2] : "";
    const double seconds = argc > 3 ? std::atof (argv[3]) : 1e9;

    std::vector<std::string> names;
    if (DIR* d = opendir (dir.c_str()))
    {
        while (auto* e = readdir (d))
        {
            std::string f = e->d_name;
            if (f.size() > 4 && f.substr (f.size() - 4) == ".wav" && f[0] != '.') names.push_back (f);
        }
        closedir (d);
    }
    std::sort (names.begin(), names.end());
    std::vector<Wav> w (names.size());
    double sr = 0; size_t n = SIZE_MAX;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (! readWav ((dir + "/" + names[i]).c_str(), w[i])) { std::fprintf (stderr, "can't read %s\n", names[i].c_str()); return 1; }
        sr = w[i].sr; n = std::min (n, w[i].mono.size());
    }
    n = std::min (n, (size_t) (seconds * sr));
    const int N = (int) names.size();
    int ref = -1;
    if (! refPart.empty()) for (int i = 0; i < N; ++i) if (names[(size_t) i].find (refPart) != std::string::npos) { ref = i; break; }

    const auto t0 = std::chrono::steady_clock::now();
    const int maxLag = (int) std::lround (sa::kMaxShiftMs * 0.001 * sr);
    sa::NetworkEstimator est;
    est.prepare (sr, maxLag, N);
    std::vector<const float*> p ((size_t) N);
    for (size_t pos = 0; pos + 512 <= n; pos += 512)
    {
        for (int i = 0; i < N; ++i) p[(size_t) i] = w[(size_t) i].mono.data() + pos;
        est.feed (p.data(), 512);
    }
    const auto edges = est.directedEdges();
    if (const char* dump = std::getenv ("SA_DUMP"))
    {
        if (FILE* f = std::fopen (dump, "w"))
        {
            for (int i = 0; i < N; ++i) std::fprintf (f, "T\t%d\t%s\n", i, names[(size_t) i].c_str());
            for (auto& e : edges) std::fprintf (f, "E\t%d\t%d\t%.3f\t%d\t%.3f\t%.3f\t%d\n", e.a, e.b, e.lag, e.polarity, e.sure, e.prominence, e.hits);
            std::fclose (f);
        }
    }
    const auto r = sa::solveSession (N, edges, ref, maxLag);
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

    int conf = 0; for (auto& e : edges) if (e.sure >= 0.66) ++conf;
    std::printf ("%s: %d tracks, %.0f s at %.0f Hz, %zu pairs measured, %d confident, analysed in %.1f s%s%s\n",
                 dir.c_str(), N, n / sr, sr, edges.size(), conf, secs, ref >= 0 ? ", REF = " : "", ref >= 0 ? names[(size_t) ref].c_str() : "");
    for (int i = 0; i < N; ++i)
    {
        const auto& t = r[(size_t) i];
        if (t.group < 0) std::printf ("  %-28s  ungrouped (left alone)        hits %4d\n", names[(size_t) i].c_str(), est.hitsOf (i));
        else std::printf ("  %-28s  group %c  delay %+9.2f smp (%+7.3f ms)  pol %+d  sure %3.0f%%%s%s  hits %4d\n", names[(size_t) i].c_str(),
                          'A' + t.group, t.delaySamples, t.delaySamples * 1000.0 / sr, t.polarity, t.sure * 100.0,
                          t.anchor ? "  ANCHOR" : "", t.clamped ? "  CLAMPED" : "", est.hitsOf (i));
    }
    if (const char* dbg = std::getenv ("SA_EDGES"))
        for (auto& e : edges)
            if (names[(size_t) e.a].find (dbg) != std::string::npos || names[(size_t) e.b].find (dbg) != std::string::npos)
                std::printf ("    edge %-24s -> %-24s lag %+9.2f pol %+d sure %.2f prom %.2f hits %d\n", names[(size_t) e.a].c_str(), names[(size_t) e.b].c_str(),
                             e.lag, e.polarity, e.sure, e.prominence, e.hits);
    return 0;
}
