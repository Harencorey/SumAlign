//==============================================================================
// AlignWav - runs SumAlign's ALIGN on two WAV files, exactly as the plug-in would hear them
// in real time (same analyzer, same stopping rules), plus a whole-file measurement for comparison.
//
//   AlignWav <reference.wav> <track.wav> [label]
//
// Prints one line: label, plug-in result (Delay ms / samples, polarity, % sure, hits, seconds it needed),
// whole-file result, and the alternate points. Multi-channel files are summed to mono, like the plug-in.
// Plain C++17, no JUCE:  g++ -O2 -std=c++17 -pthread -ISource tools/AlignWav.cpp -o AlignWav
//==============================================================================

#include "AlignAnalysis.h"

#include "AlignCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "WavIO.h"

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: AlignWav reference.wav track.wav [label]\n"); return 2; }
    Wav r, x;
    if (! readWav (argv[1], r) || ! readWav (argv[2], x)) { std::fprintf (stderr, "read failed\n"); return 1; }
    if (r.sr != x.sr) { std::fprintf (stderr, "sample rates differ\n"); return 1; }
    const size_t n = std::min (r.mono.size(), x.mono.size());
    const double sr = r.sr;
    const int maxLag = (int) std::lround (sa::kMaxShiftMs * 0.001 * sr);
    const char* label = argc > 3 ? argv[3] : argv[2];

    // 1) the plug-in's way: real-time-sized blocks, analysis pass every ~25 ms of audio, its own stopping rules
    sa::AlignAnalyzer a;
    a.setThreaded (false);
    a.prepare (sr, maxLag);
    a.start();
    const int block = 256, passEvery = std::max (1, (int) (0.025 * sr) / block);
    size_t pos = 0;
    for (int k = 0; pos + block <= n && a.isListening(); pos += block, ++k)
    {
        a.push (x.mono.data() + pos, r.mono.data() + pos, block);
        if (k % passEvery == 0) a.pump();
    }
    for (int k = 0; k < 80 && a.isListening(); ++k) a.pump();   // playback stopped: ~2 s of quiet passes
    if (a.isListening()) { a.finishNow(); a.pump(); }
    const auto st = a.getStatus();

    // 2) the whole file, for comparison
    sa::PairEstimator est;
    est.prepare (sr, maxLag);
    est.feed (x.mono.data(), r.mono.data(), (int) n);
    const auto all = est.result();

    const char* state = st.state == sa::AlignAnalyzer::State::done ? "ALIGNED" : "NOT_SURE";
    std::printf ("%-34s %-8s delay %+8.3f ms (%+9.2f smp) pol %+d  sure %3.0f%%  hits %3d  needed %5.1f s | whole song: %+9.2f smp pol %+d sure %3.0f%% hits %4d | pts:",
                 label, state, -st.result.lagSamples * 1000.0 / sr, -st.result.lagSamples, st.result.polarity,
                 st.result.confidence * 100.0, st.result.windows, st.heardSeconds,
                 -all.lagSamples, all.polarity, all.confidence * 100.0, all.windows);
    for (auto& p : all.points) std::printf (" %+.1f/%c%.2f", -p.lagSamples, p.polarity > 0 ? '+' : '-', p.strength);
    std::printf ("  [sr %.0f, %.0f s]\n", sr, n / sr);
    return 0;
}
