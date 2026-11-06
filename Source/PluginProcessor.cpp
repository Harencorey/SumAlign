#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

//==============================================================================
AudioProcessorValueTreeState::ParameterLayout SumAlignProcessor::createLayout()
{
    using P = AudioParameterFloat;
    using A = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> ps;

    auto ms = A().withLabel ("ms")
                 .withStringFromValueFunction ([] (float v, int)
                 {
                     if (std::abs (v) < 0.0005f) return String ("0.000 ms");
                     return (v > 0 ? "+" : "") + String (v, 3) + " ms";
                 })
                 .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("-+.0123456789").getFloatValue(); });

    auto deg = A().withLabel (CharPointer_UTF8 ("\xc2\xb0"))
                  .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + String (CharPointer_UTF8 ("\xc2\xb0")); })
                  .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters (".0123456789").getFloatValue(); });

    auto hz = A().withLabel ("Hz")
                 .withStringFromValueFunction ([] (float v, int)
                 { return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz" : String (roundToInt (v)) + " Hz"; })
                 .withValueFromStringFunction ([] (const String& t)
                 {
                     const float v = t.retainCharacters (".0123456789").getFloatValue();
                     return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
                 });

    const float maxMs = (float) sa::kMaxShiftMs;
    ps.push_back (std::make_unique<P> (ParameterID { sa::pid::delay, 1 }, "Delay",
                                       NormalisableRange<float> (-maxMs, maxMs), 0.0f, ms));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { sa::pid::invert, 1 }, "Polarity", false,
                                       AudioParameterBoolAttributes().withStringFromValueFunction ([] (bool b, int)
                                           { return b ? String ("Inverted") : String ("Normal"); })));
    ps.push_back (std::make_unique<P> (ParameterID { sa::pid::phase, 1 }, "Phase",
                                       NormalisableRange<float> (0.0f, 180.0f), 0.0f, deg));
    NormalisableRange<float> fc (sa::kPhaseCenterMin, sa::kPhaseCenterMax);
    fc.setSkewForCentre (400.0f);
    ps.push_back (std::make_unique<P> (ParameterID { sa::pid::phaseCenter, 1 }, "Phase Center", fc, sa::kPhaseCenterDefault, hz));
    return { ps.begin(), ps.end() };
}

//==============================================================================
SumAlignProcessor::SumAlignProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    AudioChannelSet::stereo(), true)
                          .withInput  ("Reference", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SumAlign", createLayout())
{
    raw.delay       = apvts.getRawParameterValue (sa::pid::delay);
    raw.invert      = apvts.getRawParameterValue (sa::pid::invert);
    raw.phase       = apvts.getRawParameterValue (sa::pid::phase);
    raw.phaseCenter = apvts.getRawParameterValue (sa::pid::phaseCenter);

    engine.prepare (48000.0, 2);
    setLatencySamples (engine.latencySamples());
    analyzer.prepare (48000.0, engine.maxShiftSamples());
    capX.assign (4096, 0.0f);
    capR.assign (4096, 0.0f);
    startTimerHz (15);
}

SumAlignProcessor::~SumAlignProcessor()
{
    stopTimer();
    analyzer.shutdown();
}

bool SumAlignProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo()) return false;
    if (in != out) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != AudioChannelSet::mono() && sc != AudioChannelSet::stereo()) return false;
    }
    return true;
}

sa::Settings SumAlignProcessor::readSettings() const noexcept
{
    sa::Settings s;
    double samples = (double) raw.delay->load() * 0.001 * rate.load();
    // a value that's a whole number of samples (typed in, or nudged) should be exactly that:
    // whole-sample delays are a plain copy, no interpolation at all
    const double r = std::round (samples);
    if (std::abs (samples - r) < 1.0e-3) samples = r;   // float parameter precision at 192 kHz
    s.delaySamples  = samples;
    s.invert        = raw.invert->load() > 0.5f;
    s.phaseDeg      = (double) raw.phase->load();
    s.phaseCenterHz = (double) raw.phaseCenter->load();
    return s;
}

void SumAlignProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    rate.store (sampleRate);
    engine.prepare (sampleRate, 2);
    engine.setSettings (readSettings());
    engine.reset();
    setLatencySamples (engine.latencySamples());   // fixed for a given sample rate
    analyzer.prepare (sampleRate, engine.maxShiftSamples());   // cancels a running ALIGN
    capX.assign ((size_t) jmax (4096, samplesPerBlock), 0.0f);
    capR.assign ((size_t) jmax (4096, samplesPerBlock), 0.0f);
}

void SumAlignProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, false, 0);
    const int numCh = main.getNumChannels();
    const int n = buffer.getNumSamples();

    // The sidechain view must outlive engine.process(): its channel-pointer array lives
    // inside this AudioBuffer object, so it can't be a temporary inside the if-block.
    AudioBuffer<float> sc;
    const float* const* scPtrs = nullptr;
    int scCh = 0;
    if (getBusCount (true) > 1)
    {
        auto* scBus = getBus (true, 1);
        if (scBus != nullptr && scBus->isEnabled())
        {
            sc = getBusBuffer (buffer, true, 1);
            scCh = sc.getNumChannels();
            scPtrs = scCh > 0 ? sc.getArrayOfReadPointers() : nullptr;
        }
    }

    // ALIGN listens to the raw input (before this plug-in moves it) against the raw reference
    if (analyzer.isListening() && scCh > 0 && numCh > 0)
    {
        const float* const* in = main.getArrayOfReadPointers();
        for (int pos = 0; pos < n;)
        {
            const int len = jmin (n - pos, (int) capX.size());
            for (int i = 0; i < len; ++i)
            {
                float x = 0.0f, r = 0.0f;
                for (int c = 0; c < numCh; ++c) x += in[c][pos + i];
                for (int c = 0; c < scCh; ++c)  r += scPtrs[c][pos + i];
                capX[(size_t) i] = x;
                capR[(size_t) i] = r;
            }
            analyzer.push (capX.data(), capR.data(), len);
            pos += len;
        }
    }

    engine.setSettings (readSettings());
    engine.process (main.getArrayOfWritePointers(), numCh, scPtrs, scCh, n);

    for (int c = numCh; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);
}

void SumAlignProcessor::processBlockBypassed (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    engine.processBypassed (main.getArrayOfWritePointers(), main.getNumChannels(), buffer.getNumSamples());
    for (int c = main.getNumChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
}

void SumAlignProcessor::nudgeDelaySamples (double samples)
{
    auto* p = apvts.getParameter (sa::pid::delay);
    const double ms = (double) raw.delay->load() + samples * 1000.0 / rate.load();
    const float clamped = (float) jlimit (-sa::kMaxShiftMs, sa::kMaxShiftMs, ms);
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (clamped));
    p->endChangeGesture();
}

//==============================================================================
void SumAlignProcessor::alignPressed()
{
    const auto st = analyzer.getStatus();
    if (st.state == sa::AlignAnalyzer::State::listening) analyzer.finishNow();
    else analyzer.start();
}

void SumAlignProcessor::timerCallback()
{
    const auto st = analyzer.getStatus();
    if (st.id == appliedId) return;
    appliedId = st.id;
    if (st.state != sa::AlignAnalyzer::State::done || ! st.result.valid) return;   // "not sure": change nothing

    const double r = rate.load();
    std::vector<Point> pts;
    for (const auto& p : st.result.points)
    {
        const double ms = -p.lagSamples * 1000.0 / r;
        if (std::abs (ms) <= sa::kMaxShiftMs) pts.push_back ({ ms, p.polarity < 0, p.strength });
    }
    if (pts.empty()) return;
    applyPoint (pts.front());   // the strongest
    std::sort (pts.begin(), pts.end(), [] (const Point& a, const Point& b) { return a.delayMs < b.delayMs; });
    storePoints (pts);
    apvts.state.setProperty ("alignConfidence", st.result.confidence, nullptr);
}

void SumAlignProcessor::applyPoint (const Point& p)
{
    auto set = [] (RangedAudioParameter* prm, float v)
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (v));
        prm->endChangeGesture();
    };
    set (apvts.getParameter (sa::pid::delay), (float) p.delayMs);
    set (apvts.getParameter (sa::pid::invert), p.invert ? 1.0f : 0.0f);
}

// Saved with the session (as text), so Earlier/Later still work after reopening it.
void SumAlignProcessor::storePoints (const std::vector<Point>& pts)
{
    StringArray items;
    for (auto& p : pts) items.add (String (p.delayMs, 6) + ":" + (p.invert ? "1" : "0") + ":" + String (p.strength, 3));
    apvts.state.setProperty ("alignPoints", items.joinIntoString (";"), nullptr);
}

std::vector<SumAlignProcessor::Point> SumAlignProcessor::alignPoints() const
{
    std::vector<Point> pts;
    StringArray items;
    items.addTokens (apvts.state.getProperty ("alignPoints").toString(), ";", "");
    for (auto& it : items)
    {
        StringArray f; f.addTokens (it, ":", "");
        if (f.size() == 3) pts.push_back ({ f[0].getDoubleValue(), f[1] == "1", f[2].getDoubleValue() });
    }
    return pts;
}

int SumAlignProcessor::currentPointIndex() const
{
    const auto pts = alignPoints();
    const double now = (double) raw.delay->load();
    const double tol = 0.5 * 1000.0 / rate.load();   // half a sample
    for (size_t i = 0; i < pts.size(); ++i)
        if (std::abs (pts[i].delayMs - now) <= tol) return (int) i;
    return -1;
}

void SumAlignProcessor::stepPoint (int direction)
{
    const auto pts = alignPoints();
    if (pts.empty()) return;
    const double now = (double) raw.delay->load();
    const double tol = 0.5 * 1000.0 / rate.load();
    if (direction < 0)
    {
        for (int i = (int) pts.size() - 1; i >= 0; --i)
            if (pts[(size_t) i].delayMs < now - tol) { applyPoint (pts[(size_t) i]); return; }
    }
    else
    {
        for (auto& p : pts)
            if (p.delayMs > now + tol) { applyPoint (p); return; }
    }
}

//==============================================================================
void SumAlignProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("version", JucePlugin_VersionString, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void SumAlignProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* SumAlignProcessor::createEditor() { return new SumAlignEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SumAlignProcessor(); }
