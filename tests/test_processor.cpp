//==============================================================================
// SumAlign - plug-in level tests on the real processor (bus layouts, reported latency,
// bypass, sidechain reference, parameter -> samples conversion, session save/load).
//==============================================================================

#include "PluginProcessor.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, ...) \
    do { if (! (cond)) { ++failures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

using namespace juce;

static void setParam (SumAlignProcessor& p, const char* id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static bool setLayout (SumAlignProcessor& p, AudioChannelSet main, AudioChannelSet sc)
{
    AudioProcessor::BusesLayout l;
    l.inputBuses.add (main);
    l.inputBuses.add (sc);
    l.outputBuses.add (main);
    return p.setBusesLayout (l);
}

static void testLayouts()
{
    std::printf ("Bus layouts: mono and stereo, reference sidechain off / mono / stereo\n");
    SumAlignProcessor p;
    for (auto main : { AudioChannelSet::mono(), AudioChannelSet::stereo() })
        for (auto sc : { AudioChannelSet::disabled(), AudioChannelSet::mono(), AudioChannelSet::stereo() })
            CHECK (setLayout (p, main, sc), "layout %s / %s refused", main.getDescription().toRawUTF8(), sc.getDescription().toRawUTF8());
    AudioProcessor::BusesLayout bad;
    bad.inputBuses.add (AudioChannelSet::mono());
    bad.inputBuses.add (AudioChannelSet::disabled());
    bad.outputBuses.add (AudioChannelSet::stereo());
    CHECK (! p.checkBusesLayoutSupported (bad), "mono in / stereo out should be refused");
}

static void testLatencyBypassAndDelay()
{
    std::printf ("Fixed reported latency; bypass keeps it; Delay in ms becomes exact samples\n");
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        SumAlignProcessor p;
        setLayout (p, AudioChannelSet::stereo(), AudioChannelSet::disabled());
        p.prepareToPlay (sr, 128);
        const int L = (int) std::lround (0.030 * sr) + sa::kInterpHalf;
        CHECK (p.getLatencySamples() == L, "sr %.0f latency %d, expected %d", sr, p.getLatencySamples(), L);

        // an impulse comes out exactly L (+ delay) samples later, in normal and bypassed processing
        for (int pass = 0; pass < 3; ++pass)
        {
            const int wantShift = pass == 2 ? -25 : 0;
            setParam (p, sa::pid::delay, (float) (wantShift * 1000.0 / sr));
            p.prepareToPlay (sr, 128);
            AudioBuffer<float> buf (2, 128);
            MidiBuffer midi;
            int found = -1;
            for (int blk = 0; blk < 100 && found < 0; ++blk)
            {
                buf.clear();
                if (blk == 0) { buf.setSample (0, 5, 1.0f); buf.setSample (1, 5, 1.0f); }
                if (pass == 1) p.processBlockBypassed (buf, midi); else p.processBlock (buf, midi);
                for (int i = 0; i < 128; ++i)
                    if (buf.getSample (0, i) == 1.0f && buf.getSample (1, i) == 1.0f) { found = blk * 128 + i - 5; break; }
            }
            CHECK (found == L + wantShift, "sr %.0f pass %d: impulse at %d, expected %d", sr, pass, found, L + wantShift);
        }
    }
}

static void testReferenceMeter()
{
    std::printf ("The sidechain reference drives the correlation meter\n");
    const double sr = 48000.0;
    SumAlignProcessor p;
    setLayout (p, AudioChannelSet::mono(), AudioChannelSet::mono());
    p.prepareToPlay (sr, 256);
    Random rng (3);
    AudioBuffer<float> buf (2, 256);
    MidiBuffer midi;
    for (int blk = 0; blk < 200; ++blk)
    {
        for (int i = 0; i < 256; ++i)
        {
            const float v = rng.nextFloat() - 0.5f;
            buf.setSample (0, i, -v);   // this track: the reference, inverted
            buf.setSample (1, i, v);    // sidechain: the reference
        }
        p.processBlock (buf, midi);
    }
    CHECK (p.engine.refPresent.load(), "reference not seen");
    CHECK (p.engine.correlation.load() < -0.95f, "inverted copy reads %.3f", p.engine.correlation.load());
    setParam (p, sa::pid::invert, 1.0f);
    for (int blk = 0; blk < 400; ++blk)   // the meter averages over ~300 ms
    {
        for (int i = 0; i < 256; ++i) { const float v = rng.nextFloat() - 0.5f; buf.setSample (0, i, -v); buf.setSample (1, i, v); }
        p.processBlock (buf, midi);
    }
    CHECK (p.engine.correlation.load() > 0.95f, "after Polarity reads %.3f", p.engine.correlation.load());
}

static void testState()
{
    std::printf ("Session save/load restores every parameter\n");
    SumAlignProcessor a;
    setParam (a, sa::pid::delay, -3.217f);
    setParam (a, sa::pid::invert, 1.0f);
    setParam (a, sa::pid::phase, 72.5f);
    setParam (a, sa::pid::phaseCenter, 1800.0f);
    MemoryBlock mb;
    a.getStateInformation (mb);
    SumAlignProcessor b;
    b.setStateInformation (mb.getData(), (int) mb.getSize());
    for (auto* id : { sa::pid::delay, sa::pid::invert, sa::pid::phase, sa::pid::phaseCenter })
        CHECK (std::abs (a.apvts.getRawParameterValue (id)->load() - b.apvts.getRawParameterValue (id)->load()) < 1e-6f,
               "%s not restored", id);
    // text entry round-trips
    auto* d = b.apvts.getParameter (sa::pid::delay);
    CHECK (std::abs (d->getValueForText ("-0.35 ms") - d->convertTo0to1 (-0.35f)) < 1e-6f, "typing -0.35 ms");
    auto* f = b.apvts.getParameter (sa::pid::phaseCenter);
    CHECK (std::abs (f->convertFrom0to1 (f->getValueForText ("1.2 kHz")) - 1200.0f) < 0.5f, "typing 1.2 kHz");
}

static void testAlign()
{
    std::printf ("ALIGN end to end: listens to the raw input vs the sidechain, then sets Delay + Polarity\n");
    const double sr = 48000.0;
    const double lag = 333.37;   // this mic hears the source 6.95 ms later, inverted
    SumAlignProcessor p;
    setLayout (p, AudioChannelSet::stereo(), AudioChannelSet::mono());
    setParam (p, sa::pid::delay, 4.0f);   // a wrong manual setting first: ALIGN measures the raw input anyway
    p.prepareToPlay (sr, 512);

    // drum-like source
    const int n = (int) (25 * sr);
    std::vector<float> src ((size_t) n + 4096, 0.0f);
    Random rng (11);
    for (int at = 2000; at < n; at += (int) (sr * (0.25 + 0.2 * rng.nextFloat())))
        for (int i = 0; i < (int) (0.3 * sr) && at + i < n; ++i)
        {
            const double t = i / sr;
            src[(size_t) (at + i)] += (float) (0.5 * std::exp (-t / 0.1) * std::sin (2 * MathConstants<double>::pi * 160 * t)
                                               + 0.4 * std::exp (-t / 0.004) * (rng.nextFloat() * 2 - 1));
        }
    sa::Taps tap; tap.design (lag + sa::kInterpHalf);

    p.alignPressed();
    AudioBuffer<float> buf (3, 512);
    MidiBuffer midi;
    for (int pos = 4096; pos + 512 < n; pos += 512)
    {
        for (int i = 0; i < 512; ++i)
        {
            double acc = 0;
            for (int k = 0; k < sa::kTaps; ++k)
                acc += tap.h[(size_t) k] * src[(size_t) (pos + i - tap.whole - (k - sa::kInterpHalf) + sa::kInterpHalf)];
            buf.setSample (0, i, (float) -acc); buf.setSample (1, i, (float) -acc);
            buf.setSample (2, i, src[(size_t) (pos + i)]);
        }
        p.processBlock (buf, midi);
        if ((pos / 512) % 8 == 0) Thread::sleep (1);
        if (p.alignStatus().state != sa::AlignAnalyzer::State::listening) break;
    }
    for (int i = 0; i < 300 && p.alignStatus().state == sa::AlignAnalyzer::State::listening; ++i) Thread::sleep (10);
    p.checkAlign();
    const auto st = p.alignStatus();
    CHECK (st.state == sa::AlignAnalyzer::State::done, "ALIGN state %d (conf %.2f agree %.2f peak %.1f hits %d lag %.3f)", (int) st.state, st.result.confidence, st.result.agreement, st.result.peakRatio, st.result.windows, st.result.lagSamples);
    const float wantMs = (float) (-lag * 1000.0 / sr);
    const float gotMs = p.apvts.getRawParameterValue (sa::pid::delay)->load();
    CHECK (std::abs (gotMs - wantMs) * sr / 1000.0 < 0.05, "Delay %.4f ms, want %.4f (%.3f samples off)", gotMs, wantMs, (gotMs - wantMs) * sr / 1000.0);
    CHECK (p.apvts.getRawParameterValue (sa::pid::invert)->load() > 0.5f, "Polarity not flipped");
    CHECK (p.currentPointIndex() == 0 && p.alignPoints().size() >= 1, "points: %d, current %d", (int) p.alignPoints().size(), p.currentPointIndex());
    std::printf ("  set Delay %.4f ms (want %.4f), polarity %s, %.0f %% sure after %.1f s\n", gotMs, wantMs,
                 p.apvts.getRawParameterValue (sa::pid::invert)->load() > 0.5f ? "flipped" : "normal", st.result.confidence * 100, st.heardSeconds);

    // the points survive save/load
    MemoryBlock mb; p.getStateInformation (mb);
    SumAlignProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
    q.prepareToPlay (sr, 512);
    CHECK (q.alignPoints().size() == p.alignPoints().size() && q.currentPointIndex() == 0, "points not restored");
}

// A host transport for the session test: playing, at a given timeline position
struct FakePlayHead : AudioPlayHead
{
    int64 time = 0;
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (true);
        p.setTimeInSamples (time);
        return p;
    }
};

static void testSession()
{
    std::printf ("Session ALIGN: 4 instances find each other, listen together on the timeline, each sets its own Delay/Polarity\n");
    const double sr = 48000.0;
    const int n = (int) (40 * sr);
    // one drum-like source, heard by 4 "mics": delay (samples) and polarity per mic, plus one unrelated track
    const double lag[4] = { 0.0, 37.4, 161.2, 766.0 };
    const int pol[4] = { 1, -1, 1, -1 };
    std::vector<float> src ((size_t) n + 4096, 0.0f), other ((size_t) n + 4096, 0.0f);
    Random rng (5);
    for (int at = 4000; at < n; at += (int) (sr * (0.2 + 0.3 * rng.nextFloat())))
        for (int i = 0; i < (int) (0.3 * sr) && at + i < n; ++i)
        {
            const double t = i / sr;
            src[(size_t) (at + i)] += (float) (0.5 * std::exp (-t / 0.1) * std::sin (2 * MathConstants<double>::pi * (150 + 100 * std::exp (-t / 0.01)) * t)
                                               + 0.5 * std::exp (-t / 0.004) * (rng.nextFloat() * 2 - 1));
        }
    for (int i = 0; i < n; ++i) other[(size_t) i] = 0.2f * std::sin (0.031f * (float) i) * (std::sin (0.00002f * (float) i) > 0 ? 1.0f : 0.0f);

    std::vector<std::unique_ptr<SumAlignProcessor>> procs;
    FakePlayHead head;
    for (int k = 0; k < 5; ++k)
    {
        auto p = std::make_unique<SumAlignProcessor>();
        setLayout (*p, AudioChannelSet::mono(), AudioChannelSet::disabled());
        p->setPlayHead (&head);
        p->prepareToPlay (sr, 512);
        procs.push_back (std::move (p));
    }
    CHECK (procs[0]->sessionMode(), "instances don't see each other");
    procs[2]->setRef (false);
    procs[0]->alignPressed();   // any instance starts it for all

    std::vector<sa::Taps> taps (4);
    for (int m = 0; m < 4; ++m) taps[(size_t) m].design (lag[m] + sa::kInterpHalf);
    AudioBuffer<float> buf (1, 512);
    MidiBuffer midi;
    head.time = 1000000;
    for (int pos = 4096; pos + 512 < n; pos += 512)
    {
        for (int k = 0; k < 5; ++k)
        {
            for (int i = 0; i < 512; ++i)
            {
                float v;
                if (k < 4)
                {
                    double acc = 0;
                    const auto& tp = taps[(size_t) k];
                    for (int q = 0; q < sa::kTaps; ++q)
                        acc += tp.h[(size_t) q] * src[(size_t) (pos + i - tp.whole - (q - sa::kInterpHalf) + sa::kInterpHalf)];
                    v = (float) (pol[k] * acc);
                }
                else v = other[(size_t) (pos + i)];
                buf.setSample (0, i, v);
            }
            procs[(size_t) k]->processBlock (buf, midi);
        }
        head.time += 512;
        Thread::sleep (2);
        if (procs[0]->hubStatus().state != sa::AlignHub::State::listening) break;
    }
    for (int i = 0; i < 300 && procs[0]->hubStatus().state == sa::AlignHub::State::listening; ++i) Thread::sleep (10);
    for (auto& p : procs) p->checkAlign();
    const auto hs = procs[0]->hubStatus();
    CHECK (hs.state == sa::AlignHub::State::done, "session state %d", (int) hs.state);
    for (int k = 0; k < 4; ++k)
    {
        const float want = (float) (-lag[k] * 1000.0 / sr);
        const float got = procs[(size_t) k]->apvts.getRawParameterValue (sa::pid::delay)->load();
        const bool inv = procs[(size_t) k]->apvts.getRawParameterValue (sa::pid::invert)->load() > 0.5f;
        CHECK (std::abs (got - want) * sr / 1000.0 < 0.3 && inv == (pol[k] < 0), "mic %d: Delay %.4f ms (want %.4f), inverted %d", k, got, want, (int) inv);
        std::printf ("  mic %d: Delay %+.4f ms (want %+.4f), %s\n", k, got, want, inv ? "inverted" : "normal");
    }
    CHECK (procs[4]->apvts.getRawParameterValue (sa::pid::delay)->load() == 0.0f, "unrelated track was moved");
    std::printf ("  finished after %.1f s, %d group(s); the unrelated track was left alone\n", hs.heardSeconds, hs.groups);
}

static void testEditor()
{
    std::printf ("Editor opens and closes\n");
    SumAlignProcessor p;
    p.prepareToPlay (48000.0, 256);
    std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
    CHECK (ed != nullptr && ed->getWidth() > 0, "no editor");
    ed.reset();
}

int main()
{
    ScopedJuceInitialiser_GUI juce;
    testLayouts();
    testLatencyBypassAndDelay();
    testReferenceMeter();
    testState();
    testAlign();
    testSession();
    testEditor();
    if (failures == 0) std::printf ("\nALL PROCESSOR TESTS PASSED\n");
    else               std::printf ("\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
