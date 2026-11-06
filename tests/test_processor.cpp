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
        const int L = (int) std::lround (0.010 * sr) + sa::kInterpHalf;
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
            for (int blk = 0; blk < 40 && found < 0; ++blk)
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
    testEditor();
    if (failures == 0) std::printf ("\nALL PROCESSOR TESTS PASSED\n");
    else               std::printf ("\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
