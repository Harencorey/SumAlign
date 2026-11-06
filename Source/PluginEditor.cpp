#include "PluginEditor.h"

using namespace juce;

namespace colours
{
    const Colour bg      { 0xff141518 };
    const Colour panel   { 0xff1c1e22 };
    const Colour edge    { 0xff2b2e34 };
    const Colour text    { 0xffd9dadd };
    const Colour dim     { 0xff7d828c };
    const Colour accent  { 0xffe8b04a };   // amber (Norka)
    const Colour good    { 0xff3fbf8f };   // green: summing
    const Colour bad     { 0xffe5604f };   // red: cancelling
    const Colour phase   { 0xff5aa9e6 };   // blue: phase rotation
}

//==============================================================================
SumAlignLook::SumAlignLook()
{
    setColour (Slider::textBoxTextColourId, colours::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, colours::accent.withAlpha (0.4f));
    setColour (Label::textColourId, colours::dim);
    setColour (Label::outlineColourId, Colours::transparentBlack);
    setColour (TextEditor::outlineColourId, Colours::transparentBlack);
    setColour (TextEditor::focusedOutlineColourId, colours::accent);
    setColour (TextEditor::backgroundColourId, colours::bg);
    setColour (TextEditor::textColourId, colours::text);
    setColour (TextButton::textColourOffId, colours::dim);
    setColour (TextButton::textColourOnId, colours::bg);
    setColour (TooltipWindow::backgroundColourId, colours::panel);
    setColour (TooltipWindow::textColourId, colours::text);
    setColour (TooltipWindow::outlineColourId, colours::edge);
}

void SumAlignLook::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                     float start, float end, Slider& s)
{
    const auto bounds = Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float ang = start + pos * (end - start);
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? start + (float) s.valueToProportionOfLength (0.0) * (end - start) : start;
    const auto cv = s.getProperties()["colour"];
    const Colour col = cv.isInt() ? Colour ((uint32) (int) cv) : colours::accent;

    Path track;
    track.addCentredArc (c.x, c.y, r - 3.0f, r - 3.0f, 0.0f, start, end, true);
    g.setColour (colours::edge);
    g.strokePath (track, PathStrokeType (4.0f, PathStrokeType::curved, PathStrokeType::rounded));

    if (std::abs (ang - from) > 0.001f)
    {
        Path val;
        val.addCentredArc (c.x, c.y, r - 3.0f, r - 3.0f, 0.0f, jmin (from, ang), jmax (from, ang), true);
        g.setColour (col);
        g.strokePath (val, PathStrokeType (4.0f, PathStrokeType::curved, PathStrokeType::rounded));
    }

    const float kr = r - 11.0f;
    g.setColour (colours::panel.brighter (0.08f));
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
    g.setColour (colours::edge);
    g.drawEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f, 1.0f);

    Path ptr;
    ptr.addRoundedRectangle (-1.5f, -kr + 4.0f, 3.0f, kr * 0.45f, 1.5f);
    g.setColour (colours::text);
    g.fillPath (ptr, AffineTransform::rotation (ang).translated (c.x, c.y));
}

void SumAlignLook::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool over, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    Colour fill = b.getToggleState() ? colours::accent : (over ? colours::edge.brighter (0.1f) : colours::panel);
    if (down) fill = fill.brighter (0.15f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colours::edge);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

//==============================================================================
void PhaseCurve::update()
{
    const auto s = proc.readSettings();
    const double r = proc.currentSampleRate();
    if (s.phaseDeg != last.phaseDeg || s.phaseCenterHz != last.phaseCenterHz || r != lastRate)
    {
        last = s; lastRate = r;
        repaint();
    }
}

void PhaseCurve::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (colours::bg);
    g.fillRoundedRectangle (b, 4.0f);
    const auto plot = b.reduced (6.0f, 6.0f).withTrimmedBottom (10.0f);

    auto xFor = [&] (double hz) { return plot.getX() + (float) (std::log (hz / 20.0) / std::log (1000.0)) * plot.getWidth(); };
    auto yFor = [&] (double deg) { return plot.getY() + (float) (deg / 180.0) * plot.getHeight(); };

    g.setColour (colours::edge);
    for (double hz : { 100.0, 1000.0, 10000.0 })
        g.drawVerticalLine ((int) xFor (hz), plot.getY(), plot.getBottom());
    g.drawHorizontalLine ((int) yFor (90.0), plot.getX(), plot.getRight());

    g.setColour (colours::dim);
    g.setFont (FontOptions (10.0f));
    g.drawText ("100", Rectangle<float> (xFor (100.0) - 15, plot.getBottom() + 1, 30, 10), Justification::centred);
    g.drawText ("1k", Rectangle<float> (xFor (1000.0) - 15, plot.getBottom() + 1, 30, 10), Justification::centred);
    g.drawText ("10k", Rectangle<float> (xFor (10000.0) - 15, plot.getBottom() + 1, 30, 10), Justification::centred);
    g.drawText (CharPointer_UTF8 ("0\xc2\xb0"), plot.withHeight (10).translated (2, 0), Justification::topLeft);
    g.drawText (CharPointer_UTF8 ("180\xc2\xb0"), plot.withTrimmedTop (plot.getHeight() - 10).translated (2, 0), Justification::bottomLeft);

    const double rate = lastRate > 0 ? lastRate : 48000.0;
    Path p;
    const int steps = jmax (2, (int) plot.getWidth());
    for (int i = 0; i <= steps; ++i)
    {
        const double hz = 20.0 * std::pow (1000.0, (double) i / steps);
        const double d = sa::AlignEngine::phaseShiftDegAt (last, jmin (hz, rate * 0.499), rate);
        const float px = plot.getX() + (float) i / steps * plot.getWidth(), py = yFor (jlimit (0.0, 180.0, d));
        if (i == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
    }
    g.setColour (last.phaseDeg > 0.0 ? colours::phase : colours::dim);
    g.strokePath (p, PathStrokeType (1.6f));

    // the Phase Center: where the knob's angle applies exactly
    const float cx = xFor (last.phaseCenterHz);
    g.setColour (colours::phase.withAlpha (0.5f));
    g.drawVerticalLine ((int) cx, plot.getY(), plot.getBottom());
    if (last.phaseDeg > 0.0)
    {
        g.setColour (colours::phase);
        const float cy = yFor (last.phaseDeg);
        g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
    }
}

//==============================================================================
void CorrelationMeter::update()
{
    const float c = proc.engine.correlation.load();
    const bool r = proc.engine.refPresent.load(), l = proc.engine.signalPresent.load();
    const float target = l ? c : 0.0f;
    const float before = shown;
    shown += 0.25f * (target - shown);
    if (std::abs (shown - before) > 0.001f || r != ref || l != live)
    {
        ref = r; live = l;
        repaint();
    }
}

void CorrelationMeter::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    auto bar = b.withHeight (18.0f).withY (b.getY() + 18.0f);
    g.setColour (colours::text);
    g.setFont (FontOptions (11.0f, Font::bold));
    g.drawText ("SUM vs REFERENCE", b.withHeight (14.0f), Justification::centredLeft);

    g.setColour (colours::bg);
    g.fillRoundedRectangle (bar, 3.0f);
    const float mid = bar.getCentreX();
    if (live)
    {
        const float x = mid + shown * bar.getWidth() * 0.5f;
        const auto col = shown >= 0.0f ? colours::good.interpolatedWith (colours::accent, jlimit (0.0f, 1.0f, 1.0f - shown * 1.4f))
                                       : colours::bad;
        g.setColour (col);
        g.fillRoundedRectangle (Rectangle<float>::leftTopRightBottom (jmin (mid, x), bar.getY() + 2, jmax (mid, x), bar.getBottom() - 2), 2.0f);
    }
    g.setColour (colours::edge);
    g.drawVerticalLine ((int) mid, bar.getY(), bar.getBottom());

    g.setColour (colours::dim);
    g.setFont (FontOptions (10.0f));
    const auto scale = bar.translated (0, bar.getHeight() + 1).withHeight (12.0f);
    g.drawText (CharPointer_UTF8 ("\xe2\x88\x92" "1"), scale, Justification::centredLeft);
    g.drawText ("0", scale, Justification::centred);
    g.drawText ("+1", scale, Justification::centredRight);

    const auto note = b.withTrimmedTop (bar.getBottom() - b.getY() + 16.0f);
    g.setFont (FontOptions (11.0f));
    if (! ref || ! live)
    {
        g.setColour (colours::dim);
        g.drawFittedText (! ref ? "Route the reference track (e.g. the close mic) into SumAlign's sidechain to see how this track sums with it."
                                : "Waiting for signal on both this track and the sidechain.",
                          note.toNearestInt(), Justification::topLeft, 4);
    }
    else
    {
        g.setColour (colours::text);
        g.setFont (FontOptions (20.0f, Font::bold));
        g.drawText ((shown >= 0 ? "+" : "") + String (shown, 2), note.withHeight (24.0f), Justification::centredLeft);
        g.setColour (colours::dim);
        g.setFont (FontOptions (11.0f));
        g.drawFittedText ("Higher = fuller sum. Move Delay, Polarity and Phase for the highest number, then trust your ears.",
                          note.withTrimmedTop (26.0f).toNearestInt(), Justification::topLeft, 3);
    }
}

//==============================================================================
void AlignStatusView::update()
{
    st = proc.alignStatus();
    ref = proc.referenceConnected();
    using S = sa::AlignAnalyzer::State;
    listening = st.state == S::listening;
    progress = (float) st.progress;
    String a, b;
    const double rate = proc.currentSampleRate();
    const auto& r = st.result;
    if (st.state == S::listening)
    {
        if (! ref)                        { a = "Listening... but no reference on the sidechain."; b = "Route the reference track into SumAlign's sidechain."; }
        else if (st.heardSeconds < 0.05)  { a = "Listening... press play."; b = "Play the song (the whole song is best). Click ALIGN again to finish early."; }
        else
        {
            a = "Listening... " + String (r.windows) + " hits, " + String (roundToInt (st.progress * 100.0)) + " %";
            b = r.valid ? "So far: " + String (-r.lagSamples * 1000.0 / rate, 3) + " ms, " + String (roundToInt (r.confidence * 100.0)) + " % sure. Click ALIGN to finish now."
                        : "Waiting for hits that both mics hear.";
        }
    }
    else if (st.state == S::done)
    {
        const double ms = -r.lagSamples * 1000.0 / rate;
        const double cm = std::abs (ms) * 0.1 * sa::kSpeedOfSound;
        a = String ("Aligned: ") + String (roundToInt (r.confidence * 100.0)) + " % sure (" + String (r.windows) + " hits)";
        b = "This track was " + String (std::abs (ms), 3) + " ms (" + String (cm, 1) + " cm) " + (ms <= 0 ? "late" : "early")
          + (r.polarity < 0 ? ", and inverted." : ".") + " Delay and Polarity are set; Phase is yours.";
    }
    else if (st.state == S::notSure)
    {
        if (r.windows == 0) { a = "Nothing changed: no hits heard on both tracks."; b = "Is the reference on the sidechain, and was the song playing?"; }
        else
        {
            a = "Nothing changed: only " + String (roundToInt (r.confidence * 100.0)) + " % sure.";
            b = "These two tracks don't share enough of the same sound. Try a part where both mics clearly hear the same hits.";
        }
    }
    else
    {
        a = ref ? "Press ALIGN, then play the song." : "Route the reference track (e.g. the close mic) into the sidechain.";
        b = ref ? "SumAlign listens to both tracks and sets Delay and Polarity for you." : "Then press ALIGN and play the song.";
    }
    if (a != line1 || b != line2 || listening) { line1 = a; line2 = b; repaint(); }
}

void AlignStatusView::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (colours::text);
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText (line1, b.removeFromTop (18.0f), Justification::centredLeft);
    g.setColour (colours::dim);
    g.setFont (FontOptions (11.5f));
    g.drawFittedText (line2, b.removeFromTop (30.0f).toNearestInt(), Justification::topLeft, 2);
    if (listening)
    {
        auto bar = b.removeFromTop (6.0f).withTrimmedTop (1.0f);
        g.setColour (colours::bg);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (colours::accent);
        g.fillRoundedRectangle (bar.withWidth (jmax (4.0f, bar.getWidth() * progress)), 2.0f);
    }
}

//==============================================================================
SumAlignEditor::SumAlignEditor (SumAlignProcessor& p)
    : AudioProcessorEditor (&p), proc (p), curve (p), meter (p), alignStatus (p)
{
    setLookAndFeel (&look);

    makeKnob (delay, sa::pid::delay, "DELAY",
              "Moves this track later (+) or earlier (-), up to 30 ms either way, in fractions of a sample. "
              "Shift-click and drag for fine moves; double-click for 0; click the number to type a value.", 0.0);
    makeKnob (phase, sa::pid::phase, "PHASE",
              "Rotates the phase, like the Little Labs IBP: 0 = untouched, 180 = fully flipped above the Phase Center. "
              "Use it for what a delay can't fix (DI vs amp, kick in vs out). It's an all-pass: the level never changes.", 0.0);
    makeKnob (phaseCenter, sa::pid::phaseCenter, "CENTER",
              "The frequency the Phase knob's angle is set at. Low for kick/bass/toms, higher for snare, guitars and vocals.",
              sa::kPhaseCenterDefault);
    delay.slider.getProperties().set ("colour", (int) colours::accent.getARGB());
    phase.slider.getProperties().set ("colour", (int) colours::phase.getARGB());
    phaseCenter.slider.getProperties().set ("colour", (int) colours::phase.getARGB());
    phaseCenter.slider.setSkewFactorFromMidPoint (400.0);

    delayDetail.setJustificationType (Justification::centred);
    delayDetail.setColour (Label::textColourId, colours::dim);
    delayDetail.setFont (FontOptions (12.0f));
    addAndMakeVisible (delayDetail);

    const double steps[5] = { -1.0, -0.1, 0.0, 0.1, 1.0 };
    for (int i = 0; i < 5; ++i)
    {
        const double st = steps[i];
        nudge[i].onClick = [this, st]
        {
            if (st == 0.0)
            {
                auto* prm = proc.apvts.getParameter (sa::pid::delay);
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (prm->getDefaultValue());
                prm->endChangeGesture();
            }
            else
                proc.nudgeDelaySamples (st);
        };
        nudge[i].setTooltip (st == 0.0 ? "Delay back to 0" : "Nudge the delay by " + String (std::abs (st), 1) + " sample" + (std::abs (st) == 1.0 ? "" : "s"));
        addAndMakeVisible (nudge[i]);
    }
    nudgeLabel.setText ("NUDGE (samples)", dontSendNotification);
    nudgeLabel.setJustificationType (Justification::centred);
    nudgeLabel.setFont (FontOptions (10.0f));
    addAndMakeVisible (nudgeLabel);

    invert.setClickingTogglesState (true);
    invert.setTooltip ("Flips the polarity (0 / 180 at every frequency). Switching ramps through zero, so it never clicks.");
    invertAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, sa::pid::invert, invert);
    addAndMakeVisible (invert);

    curve.setTooltip ("How much the Phase knob shifts each frequency. The dot is the Phase Center, where the angle is exactly what the knob says.");
    meter.setTooltip ("How well this track sums with the reference: +1 = perfectly in phase, 0 = unrelated, -1 = cancelling. "
                      "Measured on what you hear, after Delay, Phase and Polarity.");
    addAndMakeVisible (curve);
    addAndMakeVisible (meter);

    alignButton.setTooltip ("Press, then play the song: SumAlign listens to this track and the reference on the sidechain, "
                            "and sets Delay and Polarity when it's sure. Press again while listening to finish early.");
    alignButton.onClick = [this] { proc.alignPressed(); };
    alignButton.setColour (TextButton::textColourOffId, colours::accent);
    addAndMakeVisible (alignButton);
    earlierPoint.setTooltip ("Jump to the next point where this track also lines up with the reference, but earlier. "
                             "Other points are e.g. a room mic's reflections: still phase-coherent, a different sense of space.");
    laterPoint.setTooltip ("Jump to the next point where this track also lines up with the reference, but later "
                           "(e.g. to keep some of a room mic's natural distance while staying in phase).");
    earlierPoint.onClick = [this] { proc.stepPoint (-1); };
    laterPoint.onClick   = [this] { proc.stepPoint (+1); };
    addAndMakeVisible (earlierPoint);
    addAndMakeVisible (laterPoint);
    pointLabel.setJustificationType (Justification::centred);
    pointLabel.setFont (FontOptions (11.0f));
    addAndMakeVisible (pointLabel);
    addAndMakeVisible (alignStatus);

    setSize (760, 500);
    startTimerHz (30);
    timerCallback();
}

SumAlignEditor::~SumAlignEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SumAlignEditor::makeKnob (Knob& k, const char* id, const String& name, const String& tip, double defaultValue)
{
    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 96, 20);
    k.slider.setColour (Slider::textBoxOutlineColourId, colours::edge);
    k.slider.setColour (Slider::textBoxBackgroundColourId, colours::bg);
    k.slider.setColour (Slider::textBoxTextColourId, colours::text);
    k.slider.setTooltip (tip);
    k.att = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.slider);
    k.slider.setDoubleClickReturnValue (true, defaultValue);
    k.label.setText (name, dontSendNotification);
    k.label.setJustificationType (Justification::centred);
    k.label.setFont (FontOptions (11.0f, Font::bold));
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
}

void SumAlignEditor::timerCallback()
{
    const auto s = proc.readSettings();
    const double rate = proc.currentSampleRate();
    const double ms = s.delaySamples * 1000.0 / rate;
    const double cm = ms * 0.1 * sa::kSpeedOfSound;   // 1 ms = 34.3 cm
    const String sign = s.delaySamples > 0.0005 ? String ("+")
                      : s.delaySamples < -0.0005 ? String (CharPointer_UTF8 ("\xe2\x88\x92")) : String();
    const double smp = std::abs (s.delaySamples);
    String detail = sign + (smp == std::round (smp) ? String ((int) smp) : String (smp, 2)) + " smp   "
                  + sign + String (std::abs (cm), 1) + " cm   " + sign + String (std::abs (cm) / 2.54, 1) + " in";
    if (delayDetail.getText() != detail) delayDetail.setText (detail, dontSendNotification);

    curve.update();
    meter.update();
    alignStatus.update();

    const bool listening = proc.alignStatus().state == sa::AlignAnalyzer::State::listening;
    alignButton.setButtonText (listening ? "FINISH" : "ALIGN");
    alignButton.setToggleState (listening, dontSendNotification);
    const auto pts = proc.alignPoints();
    const int cur = proc.currentPointIndex();
    earlierPoint.setEnabled (pts.size() > 1);
    laterPoint.setEnabled (pts.size() > 1);
    String pl = pts.empty() ? String ("no ALIGN yet")
              : pts.size() == 1 ? String ("1 point")
              : cur >= 0 ? "point " + String (cur + 1) + " of " + String ((int) pts.size())
                         : String ((int) pts.size()) + " points (moved by hand)";
    if (pointLabel.getText() != pl) pointLabel.setText (pl, dontSendNotification);
}

void SumAlignEditor::paint (Graphics& g)
{
    g.fillAll (colours::bg);

    g.setColour (colours::text);
    g.setFont (FontOptions (22.0f, Font::bold));
    g.drawText ("SumAlign", 16, 10, 200, 26, Justification::centredLeft);
    g.setColour (colours::dim);
    g.setFont (FontOptions (11.0f));
    g.drawText ("NORKA DSP", 128, 16, 120, 18, Justification::centredLeft);
    g.drawText (String ("v") + JucePlugin_VersionString, getWidth() - 116, getHeight() - 22, 100, 16, Justification::centredRight);
    g.drawText ("Shift-drag = fine   |   double-click = reset   |   hover 3 s for help", 16, getHeight() - 22, 420, 16, Justification::centredLeft);

    for (auto* a : { &delayArea, &phaseArea, &outArea, &alignArea })
    {
        g.setColour (colours::panel);
        g.fillRoundedRectangle (a->toFloat(), 6.0f);
        g.setColour (colours::edge);
        g.drawRoundedRectangle (a->toFloat().reduced (0.5f), 6.0f, 1.0f);
    }
    g.setColour (colours::text);
    g.setFont (FontOptions (11.0f, Font::bold));
    g.drawText ("TIME", delayArea.getX() + 12, delayArea.getY() + 8, 100, 14, Justification::centredLeft);
    g.drawText ("PHASE", phaseArea.getX() + 12, phaseArea.getY() + 8, 100, 14, Justification::centredLeft);
    g.drawText ("POLARITY", outArea.getX() + 12, outArea.getY() + 8, 100, 14, Justification::centredLeft);
    g.drawText ("AUTO", alignArea.getX() + 12, alignArea.getY() + 8, 100, 14, Justification::centredLeft);
}

void SumAlignEditor::resized()
{
    auto r = getLocalBounds().reduced (12).withTrimmedTop (36).withTrimmedBottom (18);
    alignArea = r.removeFromBottom (92);
    r.removeFromBottom (10);
    delayArea = r.removeFromLeft (250);
    r.removeFromLeft (10);
    outArea = r.removeFromRight (196);
    r.removeFromRight (10);
    phaseArea = r;

    // TIME
    {
        auto a = delayArea.reduced (10).withTrimmedTop (18);
        delay.label.setBounds (a.removeFromTop (14));
        delay.slider.setBounds (a.removeFromTop (170).withSizeKeepingCentre (170, 170));
        delayDetail.setBounds (a.removeFromTop (20));
        a.removeFromTop (10);
        nudgeLabel.setBounds (a.removeFromTop (14));
        auto row = a.removeFromTop (26);
        const int bw = (row.getWidth() - 4 * 4) / 5;
        for (int i = 0; i < 5; ++i) { nudge[i].setBounds (row.removeFromLeft (bw)); row.removeFromLeft (4); }
    }
    // PHASE
    {
        auto a = phaseArea.reduced (10).withTrimmedTop (18);
        auto knobs = a.removeFromTop (150);
        auto left = knobs.removeFromLeft (knobs.getWidth() / 2), right = knobs;
        phase.label.setBounds (left.removeFromTop (14));
        phase.slider.setBounds (left.withSizeKeepingCentre (130, 136));
        phaseCenter.label.setBounds (right.removeFromTop (14));
        phaseCenter.slider.setBounds (right.withSizeKeepingCentre (130, 136));
        a.removeFromTop (8);
        curve.setBounds (a);
    }
    // AUTO: ALIGN
    {
        auto a = alignArea.reduced (12).withTrimmedTop (16);
        alignButton.setBounds (a.removeFromLeft (150).withSizeKeepingCentre (150, 46));
        a.removeFromLeft (16);
        auto pts = a.removeFromRight (196);
        auto row = pts.withSizeKeepingCentre (pts.getWidth(), 28).translated (0, -8);
        earlierPoint.setBounds (row.removeFromLeft (row.getWidth() / 2 - 2));
        row.removeFromLeft (4);
        laterPoint.setBounds (row);
        pointLabel.setBounds (pts.withTop (row.getBottom() + 2).withHeight (16));
        a.removeFromRight (12);
        alignStatus.setBounds (a.withSizeKeepingCentre (a.getWidth(), 56));
    }
    // POLARITY + meter
    {
        auto a = outArea.reduced (12).withTrimmedTop (18);
        invert.setBounds (a.removeFromTop (40));
        a.removeFromTop (20);
        meter.setBounds (a);
    }
}
