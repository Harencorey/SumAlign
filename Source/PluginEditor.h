#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

//==============================================================================
// A knob that drags finely when Shift is held as you click.
class FineSlider : public juce::Slider
{
public:
    FineSlider() : juce::Slider (RotaryHorizontalVerticalDrag, TextBoxBelow) {}
    void mouseDown (const juce::MouseEvent& e) override
    {
        setMouseDragSensitivity (e.mods.isShiftDown() ? 2500 : 250);
        juce::Slider::mouseDown (e);
    }
};

//==============================================================================
// Phase response of the Phase knob, 20 Hz - 20 kHz, 0 at the top, -180 deg at the bottom.
class PhaseCurve : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PhaseCurve (SumAlignProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    SumAlignProcessor& proc;
    sa::Settings last;
    double lastRate = 0.0;
};

//==============================================================================
// Correlation of the output with the reference: -1 (cancelling) .. +1 (summing).
class CorrelationMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CorrelationMeter (SumAlignProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    SumAlignProcessor& proc;
    float shown = 0.0f;
    bool ref = false, live = false;
};

//==============================================================================
// ALIGN status: progress while listening, then what it found (or why it isn't sure).
class AlignStatusView : public juce::Component
{
public:
    explicit AlignStatusView (SumAlignProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    SumAlignProcessor& proc;
    sa::AlignAnalyzer::Status st;
    bool ref = false;
    juce::String line1, line2;
    float progress = 0.0f;
    bool listening = false;
};

//==============================================================================
// Session mode: every SumAlign track in the DAW, its group and what ALIGN set.
class SessionList : public juce::Component, public juce::SettableTooltipClient
{
public:
    // why a track sits out, in words (empty when it was aligned)
    static juce::String leftAloneText (const sa::AlignHub::MemberView&);
    explicit SessionList (SumAlignProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
    int rowsHeight() const { return 22 + (int) st.members.size() * 18; }
private:
    SumAlignProcessor& proc;
    sa::AlignHub::Status st;
    juce::String sig;
};

//==============================================================================
class SumAlignLook : public juce::LookAndFeel_V4
{
public:
    SumAlignLook();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
};

//==============================================================================
class SumAlignEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SumAlignEditor (SumAlignProcessor&);
    ~SumAlignEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh() { timerCallback(); }   // what the UI timer does (used by the snapshot tool)

private:
    void timerCallback() override;

    struct Knob
    {
        FineSlider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    void makeKnob (Knob&, const char* id, const juce::String& name, const juce::String& tip, double defaultValue);

    SumAlignProcessor& proc;
    SumAlignLook look;

    Knob delay, phase, phaseCenter;
    juce::Label delayDetail;
    juce::TextButton nudge[5] { juce::TextButton { juce::CharPointer_UTF8 ("\xe2\x88\x92" "1") },
                                juce::TextButton { juce::CharPointer_UTF8 ("\xe2\x88\x92" "0.1") },
                                juce::TextButton { "0" },
                                juce::TextButton { "+0.1" },
                                juce::TextButton { "+1" } };
    juce::Label nudgeLabel;
    juce::TextButton invert { juce::CharPointer_UTF8 ("\xc3\x98  INVERT") };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invertAtt;
    PhaseCurve curve;
    CorrelationMeter meter;
    juce::TextButton alignButton { "ALIGN" };
    juce::TextButton earlierPoint { juce::CharPointer_UTF8 ("\xe2\x97\x80  EARLIER") }, laterPoint { juce::CharPointer_UTF8 ("LATER  \xe2\x96\xb6") };
    juce::Label pointLabel;
    AlignStatusView alignStatus;
    juce::TextButton refButton { "REF" };
    juce::ComboBox groupBox;   // Auto / A..H / Off
    SessionList sessionList;
    juce::Viewport sessionView;
    bool lastSession = false;
    juce::TooltipWindow tooltips { this, 3000 };

    juce::Rectangle<int> delayArea, phaseArea, outArea, alignArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SumAlignEditor)
};
