//==============================================================================
// Renders the real editor to a PNG after feeding it a kick-in / kick-out style pair
// (the reference on the sidechain), so UI changes can be reviewed as screenshots.
//   AlignSnapshot out.png [unaligned]
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>

using namespace juce;

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    const double sr = 48000;
    const bool aligned = ! (argc > 2 && String (argv[2]) == "unaligned");
    SumAlignProcessor p;
    auto set = [&] (const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
    const int lag = 61;   // this mic is ~43 cm further from the source
    set (sa::pid::phaseCenter, 120.0f);
    p.prepareToPlay (sr, 256);

    // kick-like hits into both mics; this one hears them lag samples later
    const int n = (int) (sr * 12);
    std::vector<float> src ((size_t) n + 4096, 0.0f);
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    for (int hit = 0; hit < 46; ++hit)
    {
        const int at = 2048 + hit * (int) (sr * 0.25) + (int) (u (rng) * 800.0f);
        for (int i = 0; i < (int) (sr * 0.2) && at + i < (int) src.size(); ++i)
        {
            const double t = i / sr;
            const double f = 50.0 + 90.0 * std::exp (-t / 0.02);
            src[(size_t) (at + i)] += (float) (0.6 * std::exp (-t / 0.09) * std::sin (2 * MathConstants<double>::pi * f * t)
                                               + (i < 200 ? 0.3 * u (rng) * std::exp (-t / 0.001) : 0.0));
        }
    }

    if (aligned) p.alignPressed();   // let ALIGN find it
    std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
    ed->setVisible (true);
    auto* sed = dynamic_cast<SumAlignEditor*> (ed.get());
    MidiBuffer midi;
    AudioBuffer<float> blk (4, 256);
    for (int pos = 0; pos + 256 <= n; pos += 256)
    {
        for (int i = 0; i < 256; ++i)
        {
            const size_t k = (size_t) (pos + i + 2048);
            const float me = src[k - (size_t) lag], ref = src[k];
            blk.setSample (0, i, me); blk.setSample (1, i, me);
            blk.setSample (2, i, ref); blk.setSample (3, i, ref);
        }
        p.processBlock (blk, midi);
        if ((pos / 256) % 16 == 0) Thread::sleep (2);
        p.checkAlign();
        if (sed != nullptr) sed->refresh();
    }

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    File out (argc > 1 ? String (argv[1]) : String ("snapshot.png"));
    out.deleteFile();
    FileOutputStream os (out);
    PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s  (correlation %.3f)\n", out.getFullPathName().toRawUTF8(), p.engine.correlation.load());
    return 0;
}
