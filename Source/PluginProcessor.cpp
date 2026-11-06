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

void SumAlignProcessor::prepareToPlay (double sampleRate, int)
{
    rate.store (sampleRate);
    engine.prepare (sampleRate, 2);
    engine.setSettings (readSettings());
    engine.reset();
    setLatencySamples (engine.latencySamples());   // fixed for a given sample rate
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
