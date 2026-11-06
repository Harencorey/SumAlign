#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "AlignCore.h"
#include "AlignAnalysis.h"
#include "Params.h"

class SumAlignProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    SumAlignProcessor();
    ~SumAlignProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    sa::AlignEngine engine;   // the editor reads its meter atomics

    double currentSampleRate() const noexcept { return rate.load(); }
    sa::Settings readSettings() const noexcept;

    // Moves the Delay parameter by a number of samples at the current rate (UI nudge buttons).
    void nudgeDelaySamples (double samples);

    //--- ALIGN (message thread) -----------------------------------------------------------
    // Press: start listening; press while listening: finish now with what's been heard.
    void alignPressed();
    sa::AlignAnalyzer::Status alignStatus() const { return analyzer.getStatus(); }
    bool referenceConnected() const noexcept { return engine.refPresent.load(); }

    // Alternate points from the last ALIGN (sorted by delay). Earlier/Later step through them.
    struct Point { double delayMs; bool invert; double strength; };
    std::vector<Point> alignPoints() const;
    int  currentPointIndex() const;          // the point the current Delay/Polarity sits on, or -1
    void stepPoint (int direction);           // -1 = earlier, +1 = later
    void checkAlign() { timerCallback(); }    // what the timer does (tests call it directly)

private:
    void timerCallback() override;            // applies a finished ALIGN to Delay + Polarity
    void applyPoint (const Point&);
    void storePoints (const std::vector<Point>&);

    sa::AlignAnalyzer analyzer;
    int appliedId = 0;
    std::vector<float> capX, capR;            // mono mixes for the capture (sized in prepareToPlay)

    struct Raw { std::atomic<float>* delay {}, * invert {}, * phase {}, * phaseCenter {}; } raw;
    std::atomic<double> rate { 48000.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SumAlignProcessor)
};
