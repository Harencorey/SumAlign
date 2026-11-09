//==============================================================================
// Renders the real editor to a PNG after feeding it a kick-in / kick-out style pair
// (the reference on the sidechain), so UI changes can be reviewed as screenshots.
//   AlignSnapshot out.png [unaligned]
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>

using namespace juce;

struct Head : AudioPlayHead
{
    int64 time = 0;
    Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setIsPlaying (true); p.setTimeInSamples (time); return p; }
};

// "session": 6 named instances of a kit, ALIGN on all, render the overheads' window
static int sessionSnapshot (const File& out)
{
    const double sr = 48000;
    const char* names[6] = { "Kick In", "Snare Top", "Snare Bot", "OH L", "Room L", "Vocal" };
    const double lag[6] = { 0.0, 4.8, 26.0, 166.0, 771.0, 0.0 };
    const int pol[6] = { 1, 1, -1, 1, -1, 1 };
    std::vector<std::unique_ptr<SumAlignProcessor>> ps;
    Head head;
    for (int k = 0; k < 6; ++k)
    {
        auto p = std::make_unique<SumAlignProcessor>();
        AudioProcessor::TrackProperties tp; tp.name = String (names[k]);
        p->updateTrackProperties (tp);
        p->setPlayHead (&head);
        p->prepareToPlay (sr, 256);
        ps.push_back (std::move (p));
    }
    const int n = (int) (sr * 30);
    std::vector<float> kick ((size_t) n + 8192, 0.0f), snare ((size_t) n + 8192, 0.0f);
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    for (int at = 4000; at < n; at += (int) (sr * 0.5))
        for (int i = 0; i < (int) (0.3 * sr); ++i) { const double t = i / sr; kick[(size_t) (at + i)] += (float) (0.7 * std::exp (-t / 0.12) * std::sin (2 * MathConstants<double>::pi * (55 + 80 * std::exp (-t / 0.01)) * t) + 0.3 * std::exp (-t / 0.003) * u (rng)); }
    for (int at = 4000 + (int) (0.25 * sr); at < n; at += (int) (sr * 0.5))
        for (int i = 0; i < (int) (0.3 * sr); ++i) { const double t = i / sr; snare[(size_t) (at + i)] += (float) (0.5 * std::exp (-t / 0.09) * std::sin (2 * MathConstants<double>::pi * 190 * t) + 0.5 * std::exp (-t / 0.03) * u (rng)); }
    std::unique_ptr<AudioProcessorEditor> ed (ps[3]->createEditor());
    ed->setVisible (true);
    auto* sed = dynamic_cast<SumAlignEditor*> (ed.get());
    ps[0]->alignPressed();
    AudioBuffer<float> b (4, 256);
    MidiBuffer midi;
    head.time = 480000;
    const double gk[6] = { 1.0, 0.25, 0.3, 0.4, 0.5, 0.0 }, gs[6] = { 0.1, 1.0, 0.8, 0.6, 0.5, 0.0 };
    for (int pos = 4096; pos + 256 < n; pos += 256)
    {
        for (int k = 0; k < 6; ++k)
        {
            for (int i = 0; i < 256; ++i)
            {
                const int j = pos + i - (int) lag[k];
                float v = k == 5 ? 0.2f * std::sin (0.02f * (float) (pos + i)) : pol[k] * (float) (gk[k] * kick[(size_t) j] + gs[k] * snare[(size_t) j]);
                b.setSample (0, i, v); b.setSample (1, i, v); b.setSample (2, i, 0); b.setSample (3, i, 0);
            }
            ps[(size_t) k]->processBlock (b, midi);
        }
        head.time += 256;
        if (ps[0]->hubStatus().state == sa::AlignHub::State::listening) Thread::sleep (1);
        for (auto& p : ps) p->checkAlign();
        if (sed != nullptr) sed->refresh();
        if (ps[0]->hubStatus().state == sa::AlignHub::State::done && pos > n / 2) break;
    }
    for (auto& p : ps) p->checkAlign();
    if (sed != nullptr) for (int i = 0; i < 10; ++i) sed->refresh();
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    out.deleteFile();
    FileOutputStream os (out);
    PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s (session state %d)\n", out.getFullPathName().toRawUTF8(), (int) ps[0]->hubStatus().state);
    ed.reset();
    return 0;
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    if (argc > 2 && String (argv[2]) == "session") return sessionSnapshot (File (String (argv[1])));
    const double sr = 48000;
    const bool aligned = ! (argc > 2 && String (argv[2]) == "unaligned");
    SumAlignProcessor p;
    auto set = [&] (const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
    const int lag = 61;   // this mic is ~43 cm further from the source
    set (sa::pid::phaseCenter, 120.0f);
    p.prepareToPlay (sr, 256);

    // kick-like hits into both mics; this one hears them lag samples later
    const int n = (int) (sr * 20);
    std::vector<float> src ((size_t) n + 4096, 0.0f);
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    for (int hit = 0; hit < 78; ++hit)
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
        if (p.alignStatus().state == sa::AlignAnalyzer::State::listening) Thread::sleep (1);   // ~5x real time while it listens
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
