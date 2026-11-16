// Headless checks (run locally and in CI):
//  1) profiles: the built-in profile loads, bad JSON is rejected
//  2) analysis: on a boxy, ringing, distorted test riff it finds the planted resonances,
//     cuts the boxy low-mids, and the result lands closer to the Metal / Rhythm Guitar target
//  3) DSP: transparent when everything is off, COMPARE = the dry track (latency aligned),
//     MATCH holds the level, silence stays silent, AMOUNT scales the moves
//  4) state: everything comes back after save / load, user overrides are remembered
//  5) renders the editor to PNGs
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
    if (! ok) ++failures;
}

static bool writePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    if (auto os = f.createOutputStream()) return juce::PNGImageFormat().writeImageToStream (img, *os);
    return false;
}

static void setP (ServantProcessor& p, const juce::String& id, float v)
{
    auto* rp = p.apvts.getParameter (id);
    rp->setValueNotifyingHost (rp->convertTo0to1 (v));
}
static float getP (ServantProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

using Gen = std::function<float (int64_t n, int ch)>;

// A distorted power-chord riff through a boxy, ringing "cabinet":
// +8 dB at 400 Hz (boxy), rings at 900 Hz and 2.5 kHz, plus a little noise.
struct Riff
{
    double sr;
    svt::Biquad box, ring1, ring2, air;
    int64_t made = -1;
    float last[2] {};
    explicit Riff (double rate) : sr (rate)
    {
        box.peak (sr, 400, 0.8, 8.0);
        ring1.peak (sr, 900, 10.0, 9.0);
        ring2.peak (sr, 2500, 12.0, 12.0);
        air.lowpass (sr, 7000, 0.7);
    }
    float sample (int64_t n, int ch)
    {
        if (n != made)
        {
            made = n;
            static const double roots[] = { 82.41, 98.0, 110.0, 130.81, 146.83, 123.47, 92.5, 103.83 };
            const double t = (double) n / sr;
            const int note = (int) (t / 0.25) % 8;
            const double f = roots[note], tn = std::fmod (t, 0.25);
            auto saw = [] (double ph) { return 2.0 * (ph - std::floor (ph + 0.5)); };
            const double env = std::exp (-tn * 3.0) * 0.6 + 0.4;
            double x = 0.5 * saw (f * t) + 0.35 * saw (1.4983 * f * t) + 0.25 * saw (2.0 * f * t + 0.3);
            x = std::tanh (4.0 * x * env);
            uint32_t h = (uint32_t) n * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            x += 0.02 * ((double) (h & 0xffff) / 32768.0 - 1.0);
            for (int c = 0; c < 2; ++c)
            {
                float v = air.process ((float) x, c);
                v = box.process (v, c); v = ring1.process (v, c); v = ring2.process (v, c);
                last[c] = 0.12f * v;
            }
        }
        return last[ch];
    }
};

struct Render { std::vector<float> l, r; };

static Render render (ServantProcessor& p, const Gen& g, double seconds, double skipSec, double sr = 48000.0, bool prepare = true)
{
    if (prepare) p.prepareToPlay (sr, 512);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    Render out;
    const int64_t total = (int64_t) (seconds * sr), skip = (int64_t) (skipSec * sr);
    for (int64_t pos = 0; pos < total; pos += 512)
    {
        for (int i = 0; i < 512; ++i)
            for (int c = 0; c < 2; ++c) buf.setSample (c, i, g (pos + i, c));
        p.processBlock (buf, midi);
        if (pos >= skip)
        {
            out.l.insert (out.l.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512);
            out.r.insert (out.r.end(), buf.getReadPointer (1), buf.getReadPointer (1) + 512);
        }
    }
    return out;
}

static double rmsDb (const std::vector<float>& x)
{
    double s = 0; for (auto v : x) s += (double) v * v;
    return 10.0 * std::log10 (s / std::max<size_t> (1, x.size()) + 1e-30);
}

// the long-term spectrum of a rendered signal, analysed exactly the way the plugin does it
static std::vector<float> ltasOf (const Render& r, double sr)
{
    svt::Analyzer a; a.prepare (sr); a.start();
    a.push (r.l.data(), r.r.data(), (int) r.l.size());
    std::vector<float> out, env; a.getAverage (out, env);
    return out;
}

static float peakProminenceNear (const std::vector<float>& pow, double sr, float hz)
{
    const double binHz = sr / svt::kFftSize;
    const int c = (int) std::round (hz / binHz);
    float best = -100;
    for (int i = c - 4; i <= c + 4; ++i) best = std::max (best, svt::toDb (pow[(size_t) i]));
    double s = 0; int n = 0;
    for (int i = (int) (hz * 0.8 / binHz); i <= (int) (hz * 1.25 / binHz); ++i) if (std::abs (i - c) > 6) { s += pow[(size_t) i]; ++n; }
    return best - svt::toDb (s / n);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File outDir (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File::getCurrentWorkingDirectory());
    outDir.createDirectory();
    const double sr = 48000.0;

    //==== 1) profiles
    {
        ServantProcessor p;
        const auto* prof = p.getLibrary().find ("Metal", "Rhythm Guitar");
        check (prof != nullptr, "built-in Metal / Rhythm Guitar profile loads");
        if (prof != nullptr)
        {
            double mean = 0; int n = 0;
            for (int k = 0; k < svt::kBands; ++k) if (svt::bandHz (k) >= 99 && svt::bandHz (k) <= 8100) { mean += prof->target[k]; ++n; }
            check (std::abs (mean / n) < 0.01, "profile target is normalised");
            check (prof->hpf == 80.0f && prof->lpf == 12000.0f, "profile HPF / LPF read");
        }
        svt::Profile bad;
        check (! svt::Profile::fromString ("{\"format\":\"nope\"}", bad), "a non-profile JSON is rejected");
        check (! svt::Profile::fromString ("{\"format\":\"castle-danger-profile\",\"genre\":\"X\",\"source\":\"Y\",\"curve\":[[100,0],[50,0]]}", bad),
               "a profile with frequencies out of order is rejected");
    }

    if (argc > 2)   // optional: a profile JSON written by tools/build_profile.py
    {
        svt::Profile built;
        const bool ok = svt::Profile::fromString (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]).loadFileAsString(), built);
        check (ok, "profile from tools/build_profile.py loads (" + built.key() + ")");
    }

    //==== 2) analysis on the test riff
    Riff riff (sr);
    Gen riffGen = [&riff] (int64_t n, int c) { return riff.sample (n, c); };
    ServantProcessor proc;
    setP (proc, "match", 0);
    const auto before = render (proc, riffGen, 8.0, 0.0);
    const auto ltasBefore = ltasOf (before, sr);   // processed with nothing decided yet (just the default HPF)

    proc.startAnalysis();
    check (proc.getStage() == ServantProcessor::Stage::listening, "ANALYZE starts listening");
    riff = Riff (sr);
    render (proc, riffGen, 12.0, 0.0);
    proc.finishAnalysis();
    const auto& a = proc.getAnalysis();
    check (a.valid && proc.getStage() == ServantProcessor::Stage::done, "analysis finishes with a result");
    check (std::abs (a.seconds - 12.0f) < 0.6f, "it counted ~12 s of audio (" + juce::String (a.seconds, 1) + " s)");

    juce::String resList; bool found900 = false, found2500 = false;
    for (int i = 0; i < a.nRes; ++i)
    {
        resList << juce::String (a.res[i].f, 0) << " Hz (+" << juce::String (a.res[i].prom, 1) << ")  ";
        if (std::abs (a.res[i].f / 900.0f - 1.0f) < 0.03f) found900 = true;
        if (std::abs (a.res[i].f / 2500.0f - 1.0f) < 0.03f) found2500 = true;
    }
    std::cout << "       resonances: " << resList << std::endl;
    check (found900 && found2500, "finds the planted 900 Hz and 2.5 kHz rings");
    check (a.nRes <= 6, "doesn't flag every riff harmonic as a resonance (" + juce::String (a.nRes) + " found)");

    juce::String toneList; bool boxCut = false;
    for (int i = 0; i < a.nTone; ++i)
    {
        toneList << juce::String (a.tone[i].f, 0) << " Hz " << juce::String (a.tone[i].g, 1) << " dB Q" << juce::String (a.tone[i].q, 2) << "  ";
        if (a.tone[i].f > 250 && a.tone[i].f < 650 && a.tone[i].g < -2.0f) boxCut = true;
    }
    std::cout << "       tone EQ: " << toneList << std::endl;
    std::cout << "       excess outside tolerance: " << a.excessBefore << " dB -> " << a.excessAfter << " dB" << std::endl;
    check (boxCut, "tone EQ cuts the boxy low-mids");
    check (a.excessAfter < a.excessBefore * 0.5f, "tone EQ halves the error outside the profile's tolerance");
    bool knobsMatch = true;
    for (int i = 0; i < a.nTone; ++i)
        knobsMatch = knobsMatch && std::abs (getP (proc, ServantProcessor::toneId ("g", i)) - a.tone[i].g) < 0.02f
                                && std::abs (getP (proc, ServantProcessor::toneId ("f", i)) / a.tone[i].f - 1.0f) < 0.01f;
    check (knobsMatch, "the decisions land on the knobs");
    check (getP (proc, "lpf") == 12000.0f, "clean-up takes the profile's low-pass");
    check (! proc.isUserSet (0), "decided knobs aren't marked as user-set");

    // process the riff with the decisions and measure the result
    riff = Riff (sr);
    const auto after = render (proc, riffGen, 8.0, 1.0, sr, false);
    const auto ltasAfter = ltasOf (after, sr);
    const auto* prof = proc.currentProfile();
    float cb[svt::kBands], ca[svt::kBands], eb[svt::kBands], ea[svt::kBands];
    svt::bandCurve (ltasBefore, sr, cb); svt::bandCurve (ltasAfter, sr, ca);
    svt::excessCurve (cb, *prof, eb); svt::excessCurve (ca, *prof, ea);
    std::cout << "       processed riff, error vs profile: " << svt::rmsOf (eb) << " dB -> " << svt::rmsOf (ea) << " dB" << std::endl;
    check (svt::rmsOf (ea) < svt::rmsOf (eb) * 0.6f, "processed audio is much closer to the genre target");
    const float p25b = peakProminenceNear (ltasBefore, sr, 2500), p25a = peakProminenceNear (ltasAfter, sr, 2500);
    const float p9b = peakProminenceNear (ltasBefore, sr, 900), p9a = peakProminenceNear (ltasAfter, sr, 900);
    std::cout << "       2.5 kHz ring " << p25b << " -> " << p25a << " dB, 900 Hz ring " << p9b << " -> " << p9a << " dB" << std::endl;
    check (p25a < p25b - 2.5f && p9a < p9b - 2.5f, "the resonance suppressor tames both rings at the default depth");
    float maxGr = 0; for (int i = 0; i < a.nRes; ++i) maxGr = std::max (maxGr, proc.getEngine().grDb[(size_t) i].load());
    check (maxGr > 1.0f && maxGr <= 12.0f, "live resonance gain reduction is reported (" + juce::String (maxGr, 1) + " dB)");

    //==== 3) DSP behaviour
    {
        // everything off = a clean 5 ms delay
        ServantProcessor p;
        for (auto* id : { "cleanOn", "resOn", "toneOn", "match" }) setP (p, id, 0);
        Riff rf (sr); Gen gn = [&rf] (int64_t n, int c) { return rf.sample (n, c); };
        const auto out = render (p, gn, 1.0, 0.0);
        Riff ref (sr);
        const int lat = p.getLatencySamples();
        double err = 0;
        for (size_t i = (size_t) lat; i < out.l.size(); ++i) err = std::max (err, (double) std::abs (out.l[i] - ref.sample ((int64_t) i - lat, 0)));
        check (lat == 240, "reports 5 ms lookahead latency at 48 kHz (" + juce::String (lat) + ")");
        check (err < 1e-6, "transparent with every module off (max error " + juce::String (err, 9) + ")");
    }
    {
        // COMPARE = dry, aligned
        setP (proc, "compare", 1);
        Riff rf (sr); Gen gn = [&rf] (int64_t n, int c) { return rf.sample (n, c); };
        const auto out = render (proc, gn, 1.0, 0.0);
        Riff ref (sr);
        const int lat = proc.getLatencySamples();
        double err = 0;
        for (size_t i = (size_t) lat; i < out.l.size(); ++i) err = std::max (err, (double) std::abs (out.l[i] - ref.sample ((int64_t) i - lat, 0)));
        check (err < 1e-6, "COMPARE plays the dry track, latency aligned");
        setP (proc, "compare", 0);
    }
    {
        // MATCH keeps the level
        setP (proc, "match", 1);
        Riff rf (sr); Gen gn = [&rf] (int64_t n, int c) { return rf.sample (n, c); };
        const auto out = render (proc, gn, 10.0, 6.0);
        Riff ref (sr);
        std::vector<float> in; for (int64_t i = (int64_t) (6.0 * sr); i < (int64_t) (10.0 * sr); ++i) in.push_back (ref.sample (i, 0));
        const double d = rmsDb (out.l) - rmsDb (in);
        check (std::abs (d) < 1.0, "MATCH keeps the output level with the input (" + juce::String (d, 2) + " dB)");
    }
    {
        Gen silence = [] (int64_t, int) { return 0.0f; };
        const auto out = render (proc, silence, 1.0, 0.0);
        float mx = 0; for (auto v : out.l) mx = std::max (mx, std::abs (v));
        check (mx < 1e-6f, "silence stays silent");
    }
    {
        // AMOUNT 0 leaves only the clean-up filters
        setP (proc, "amount", 0); setP (proc, "match", 0);
        Riff rf (sr); Gen gn = [&rf] (int64_t n, int c) { return rf.sample (n, c); };
        const auto out = render (proc, gn, 1.0, 0.2);
        float mg = 0; for (int i = 0; i < a.nRes; ++i) mg = std::max (mg, proc.getEngine().grDb[(size_t) i].load());
        check (mg < 0.01f, "AMOUNT 0 turns the resonance suppressor off");
        setP (proc, "amount", 100);
    }

    //==== 4) state
    {
        setP (proc, ServantProcessor::toneId ("g", 0), 3.5f);   // a user override
        check (proc.isUserSet (0), "a knob moved by hand is marked as user-set");
        proc.getEngine().muted[0] = true;
        juce::MemoryBlock mb; proc.getStateInformation (mb);
        ServantProcessor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        for (auto* prm : proc.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                same = same && std::abs (r->getValue() - q.apvts.getParameter (r->getParameterID())->getValue()) < 1e-4f;
        check (same, "every knob comes back after save / load");
        int n1 = 0, n2 = 0; proc.getAppliedRes (n1); q.getAppliedRes (n2);
        check (n1 == n2 && q.getEngine().muted[0].load(), "resonances and mutes come back");
        check (q.getAnalysis().valid && q.isUserSet (0) && ! q.isUserSet (1), "the analysis and user overrides come back");
        check (std::abs (q.getAnalysis().excessBefore - a.excessBefore) < 0.01f, "the saved analysis matches");
    }

    //==== 5) GUI
    {
        auto* ed = proc.createEditor();
        ed->setSize (std::max (ed->getWidth(), 400), std::max (ed->getHeight(), 300));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        check (writePng (img, outDir.getChildFile ("gui.png")), "rendered gui.png");
        delete ed;
    }
    {
        ServantProcessor fresh;
        auto* ed = fresh.createEditor();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), outDir.getChildFile ("gui_fresh.png")), "rendered gui_fresh.png");
        delete ed;
    }

    std::cout << "\n" << failures << " failure(s)" << std::endl;
    return failures == 0 ? 0 : 1;
}
