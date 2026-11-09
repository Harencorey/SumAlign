#pragma once
// Minimal WAV reader (PCM 16/24/32, float 32), summed to mono. For the command-line tools.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

struct Wav { double sr = 0; std::vector<float> mono; };

inline bool readWav (const char* path, Wav& out)
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

