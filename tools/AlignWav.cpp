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

struct Wav { double sr = 0; std::vector<float> mono; };

static bool readWav (const char* path, Wav& out)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { std::fprintf (stderr, "can't open %s\n", path); return false; }
    std::vector<char> b ((std::istreambuf_iterator<char> (f)), {});
    if (b.size() < 12 || std::memcmp (b.data(), "RIFF", 4) || std::memcmp (b.data() + 8, "WAVE", 4)) return false;
    auto u16 = [&] (size_t i) { uint16_t v; std::memcpy (&v, &b[i], 2); return v; };
    auto u32 = [&] (size_t i) { uint32_t v; std::memcpy (&v, &b[i], 4); return v; };
    size_t i = 12, dataPos = 0, dataLen = 0;
    int fmtTag = 0, ch = 0, bits = 0; uint32_t sr = 0;
    while (i + 8 <= b.size())
    {
        const uint32_t sz = u32 (i + 4);
        if (! std::memcmp (&b[i], "fmt ", 4)) { fmtTag = u16 (i + 8); ch = u16 (i + 10); sr = u32 (i + 12); bits = u16 (i + 22);
                                                 if (fmtTag == 0xFFFE && sz >= 26) fmtTag = u16 (i + 32); }
        else if (! std::memcmp (&b[i], "data", 4)) { dataPos = i + 8; dataLen = std::min<size_t> (sz, b.size() - i - 8); }
        i += 8 + sz + (sz & 1);
    }
    if (! dataPos || ! ch || ! bits) return false;
    const int bps = bits / 8, frame = bps * ch;
    const size_t n = dataLen / (size_t) frame;
    out.sr = sr;
    out.mono.assign (n, 0.0f);
    for (size_t k = 0; k < n; ++k)
    {
        float s = 0;
        for (int c = 0; c < ch; ++c)
        {
            const char* p = &b[dataPos + k * (size_t) frame + (size_t) (c * bps)];
            float v = 0;
            if (fmtTag == 3 && bits == 32) { float x; std::memcpy (&x, p, 4); v = x; }
            else if (bits == 16) { int16_t x; std::memcpy (&x, p, 2); v = x / 32768.0f; }
            else if (bits == 24) { int32_t x = (uint8_t) p[0] | ((uint8_t) p[1] << 8) | ((int8_t) p[2] << 16); v = x / 8388608.0f; }
            else if (bits == 32) { int32_t x; std::memcpy (&x, p, 4); v = (float) (x / 2147483648.0); }
            s += v;
        }
        out.mono[k] = s;
    }
    return true;
}

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
