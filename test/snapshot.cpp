// Headless checks (run locally and in CI):
//  1) role rules: the clean-up lands inside each role's range, from where the content starts / ends
//  2) analysis (run by The Master): a boxy, ringing riff: it finds the planted resonances, decides
//     the clean-up from the track and its ROLE, makes no tone moves of its own
//  3) DSP: transparent when everything is off, COMPARE = the dry track (latency aligned),
//     MATCH holds the level, silence stays silent, AMOUNT scales the moves
//  4) state: everything comes back after save / load, user overrides are remembered
//  5) renders the editor to PNGs
#include <map>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/PluginProcessor.h"
#include "../Source/CDLink.h"
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
    bool dirty;
    double pitch;
    svt::Biquad box, ring1, ring2, air;
    int64_t made = -1;
    float last[2] {};
    explicit Riff (double rate, bool boxyAndRinging = true, double transpose = 1.0) : sr (rate), dirty (boxyAndRinging), pitch (transpose)
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
            const double f = roots[note] * pitch, tn = std::fmod (t, 0.25);
            auto saw = [] (double ph) { return 2.0 * (ph - std::floor (ph + 0.5)); };
            const double env = std::exp (-tn * 3.0) * 0.6 + 0.4;
            double x = 0.5 * saw (f * t) + 0.35 * saw (1.4983 * f * t) + 0.25 * saw (2.0 * f * t + 0.3);
            x = std::tanh (4.0 * x * env);
            uint32_t h = (uint32_t) n * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            x += 0.02 * ((double) (h & 0xffff) / 32768.0 - 1.0);
            for (int c = 0; c < 2; ++c)
            {
                float v = air.process ((float) x, c);
                if (dirty) { v = box.process (v, c); v = ring1.process (v, c); v = ring2.process (v, c); }
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

    //==== 1) role rules
    {
        float flat[svt::kBands];
        for (int k = 0; k < svt::kBands; ++k) flat[k] = svt::bandHz (k) < 140.0f ? -40.0f : (svt::bandHz (k) > 6000.0f ? -50.0f : 0.0f);
        const auto lead = svt::decideCleanup (flat, 0), rhythm = svt::decideCleanup (flat, 1), bass = svt::decideCleanup (flat, 2);
        check (lead.hpf >= 90.0f && lead.hpf <= 180.0f && lead.lpf > 19900.0f, "LEAD: high-pass from where it starts, no low-pass (" + juce::String (lead.hpf, 0) + " Hz)");
        check (rhythm.lpf >= 8000.0f && rhythm.lpf <= 12000.0f, "RHYTHM: low-pass above where the content ends (" + juce::String (rhythm.lpf, 0) + " Hz)");
        check (bass.hpf <= 45.0f && bass.lpf > 19900.0f, "BASS keeps its low end (" + juce::String (bass.hpf, 0) + " Hz)");
        const auto ch = svt::cleanupBandChange (48000.0, 80.0f, 20000.0f, 150.0f, 9000.0f);
        check (ch[0] < -10.0f && ch[1] < -3.0f && std::abs (ch[5]) < 0.2f && ch[9] < -2.0f, juce::String::formatted ("a filter change is worked out per Master band (sub %.1f, mid %.2f, air %.1f dB)", ch[0], ch[5], ch[9]));
    }

    ServantProcessor proc;
    setP (proc, "match", 0);
    setP (proc, "role", 1.0f);   // Rhythm

    //==== 2) analysis on the test riff (The Master's RESET -> play -> PRESCRIBE)
    Riff riff (sr);
    Gen riffGen = [&riff] (int64_t n, int c) { return riff.sample (n, c); };
    const auto before = render (proc, riffGen, 8.0, 0.0);
    const auto ltasBefore = ltasOf (before, sr);   // processed with nothing decided yet (just the default HPF)

    check (proc.getStage() == ServantProcessor::Stage::listening, "a new Servant is listening from the start (like The Master)");
    proc.startAnalysis();
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
    {
        float mx = 0; for (int i = 0; i < svt::kTone; ++i) mx = std::max (mx, std::abs (getP (proc, ServantProcessor::toneId ("g", i))));
        check (a.nTone == 0 && mx < 0.05f, juce::String::formatted ("no tone moves of its own: The Master decides the tone from the mix (%d moves, max %.3f dB)", a.nTone, mx));
    }
    const auto& cl = proc.getCleanup();
    std::cout << "       clean-up: " << cl.why << std::endl;
    check (std::abs (getP (proc, "hpf") - cl.hpf) < 0.5f && std::abs (getP (proc, "lpf") - cl.lpf) < 1.0f && cl.hpf >= 70.0f && cl.hpf <= 160.0f
               && cl.lpf >= 8000.0f && cl.lpf <= 12000.0f,
           "clean-up decided from the track and its ROLE lands on the knobs (" + juce::String (getP (proc, "hpf"), 0) + " Hz / " + juce::String (getP (proc, "lpf"), 0) + " Hz)");
    check (! proc.isUserSet (ServantProcessor::kHpfFlag), "decided knobs aren't marked as user-set");

    // a new ROLE re-decides from what was heard, no need to play again
    setP (proc, "role", 0.0f);   // Lead
    juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
    check (getP (proc, "lpf") > 19900.0f, "changing the ROLE re-decides the clean-up (a lead gets no low-pass)");
    setP (proc, "role", 1.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
    check (getP (proc, "lpf") < 19900.0f, "...and back for a rhythm part");

    // process the riff with the decisions: the rings are tamed
    riff = Riff (sr);
    const auto after = render (proc, riffGen, 8.0, 1.0, sr, false);
    const auto ltasAfter = ltasOf (after, sr);
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
        check (err < 1e-4, "transparent with every module off (max error " + juce::String (err, 9) + ", under -80 dB)");
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
        check (err < 1e-4, "COMPARE plays the dry track, latency aligned (max error " + juce::String (err, 9) + ")");
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
        check (std::abs (q.getCleanup().hpf - proc.getCleanup().hpf) < 0.01f && std::abs (q.getCleanup().lpf - proc.getCleanup().lpf) < 0.01f, "the clean-up decision comes back");
    }


    //==== MIX LINK: compressor, trim after MATCH, band meter, The Master setting the track over the link
    {
        auto hit = [] (int64_t n, int ch)   // snare-ish hits every 250 ms, alternating loud and 12 dB softer, on a quiet bed
        {
            const double t = (double) (n % 12000) / 48000.0;
            const double amp = (n / 12000) % 2 == 0 ? 1.0 : 0.25;
            uint32_t h = (uint32_t) (n * 2 + ch) * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            const float nz = (float) (h & 0xffff) / 32768.0f - 1.0f;
            return (float) (amp * 0.6 * std::exp (-t * 30.0) * nz + 0.05 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) n / 48000.0));
        };
        // loud-hit vs soft-hit peak level: compression should bring them closer
        ServantProcessor p;
        const int lat = p.getLatencySamples() > 0 ? p.getLatencySamples() : 240;
        auto spread = [lat] (const Render& r)
        {
            std::map<size_t, float> pk;   // rendering starts at sample 96256 (2 s, rounded up to a 512 block); the plugin delays by 'lat'
            for (size_t i = 0; i < r.l.size(); ++i) { auto& v = pk[(i + 96256 - (size_t) lat) / 12000]; v = std::max (v, std::abs (r.l[i])); }
            double a = 0, b = 0; int na = 0, nb = 0;
            for (auto& [slot, v] : pk) { if (slot % 2 == 0) { a += 20.0 * std::log10 (v); ++na; } else { b += 20.0 * std::log10 (v); ++nb; } }
            return (float) (a / std::max (1, na) - b / std::max (1, nb));
        };
        for (auto* id : { "cleanOn", "resOn", "toneOn", "match" }) setP (p, id, 0);
        const auto dry = render (p, hit, 6.0, 2.0);
        setP (p, "compOn", 1); setP (p, "comp", 70);
        const auto comp = render (p, hit, 6.0, 2.0);
        check (spread (comp) < spread (dry) - 3.0f && spread (comp) > 0.5f, juce::String::formatted ("COMPRESSOR evens out loud and soft hits (%.1f -> %.1f dB apart)", spread (dry), spread (comp)));
        setP (p, "compOn", 0); setP (p, "match", 1); setP (p, "trim", -6.0f);
        const auto trimmed = render (p, hit, 6.0, 2.0);
        check (std::abs ((rmsDb (trimmed.l) - rmsDb (dry.l)) + 6.0) < 0.5, juce::String::formatted ("TRIM -6 dB really lowers the track, even with MATCH on (%+.2f dB)", rmsDb (trimmed.l) - rmsDb (dry.l)));
        setP (p, "trim", 0.0f);

        // band meter: a 300 Hz tone lands in the 250-500 band
        ServantProcessor q;
        for (auto* id : { "cleanOn", "resOn", "toneOn", "match" }) setP (q, id, 0);
        render (q, [] (int64_t n, int) { return 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * (double) n / 48000.0); }, 3.0, 0.0);
        auto& bm = q.getBandMeter();
        int top = 0; for (int b = 1; b < 10; ++b) if (bm.out[(size_t) b].load() > bm.out[(size_t) top].load()) top = b;
        check (top == 3 && bm.outSeconds.load() > 2.5, juce::String::formatted ("band meter: 330 Hz reads in the 250-500 Hz band (band %d, %.1f s)", top, bm.outSeconds.load()));

        // over the link (The Master in "another plugin binary")
        setP (q, "role", 1.0f);
        auto other = cdlink::Node::createIsolatedForTesting();
        juce::var lastStatus, lastApplied;
        cdlink::Endpoint master ("master", [&] (const juce::var& m)
        {
            const auto t = m.getProperty ("type", "").toString();
            if (t == "status" && m.getProperty ("from", "").toString() == q.getLink().getId()) lastStatus = m;
            if (t == "applied" && m.getProperty ("from", "").toString() == q.getLink().getId()) lastApplied = m;
        }, other);
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
        for (int i = 0; i < 30 && ! master.isOnline(); ++i) pump (50);
        pump (1600);
        const auto ex = lastStatus.getProperty ("extra", {});
        check (ex.getProperty ("role", "").toString() == "Rhythm" && ex.getProperty ("bandPow", {}).getArray() != nullptr
               && ex.getProperty ("bandPow", {}).getArray()->size() == 10, "The Servant reports its role and its 10-band energy over the link");
        auto set = cdlink::Endpoint::makeMessage ("set");
        auto* prm = new juce::DynamicObject();
        prm->setProperty ("toneOn", 1.0); prm->setProperty ("t1f", 355.0); prm->setProperty ("t1g", -2.5); prm->setProperty ("t1q", 1.4);
        prm->setProperty ("compOn", 1.0); prm->setProperty ("comp", 35.0); prm->setProperty ("trim", -1.5);
        set->setProperty ("params", juce::var (prm));
        master.send (set, q.getLink().getId());
        for (int i = 0; i < 40 && ! lastApplied.isObject(); ++i) pump (50);
        check (std::abs (q.apvts.getRawParameterValue ("t1g")->load() + 2.5f) < 0.02f && std::abs (q.apvts.getRawParameterValue ("trim")->load() + 1.5f) < 0.02f
               && q.apvts.getRawParameterValue ("compOn")->load() > 0.5f && lastApplied.isObject(), "The Master sets the track's tone band, compressor and trim");
        check (! q.isUserSet (0), "The Master's settings aren't shown as your overrides");
        auto reset = cdlink::Endpoint::makeMessage ("measure-reset");
        master.send (reset);
        pump (300);
        render (q, [] (int64_t, int) { return 0.0f; }, 0.05, 0.0, 48000.0, false);
        check (q.getBandMeter().outSeconds.load() < 0.5 && q.getStage() == ServantProcessor::Stage::listening,
               "The Master's RESET restarts the track measurement and the listening (in step with its own analysis)");

        // PRESCRIBE: The Master asks it to finish; it decides, rescales what it measured, says it's done
        setP (q, "cleanOn", 1.0f); setP (q, "role", 0.0f);   // a lead that only plays up from 330 Hz
        render (q, [] (int64_t n, int) { return 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * (double) n / 48000.0); }, 4.0, 0.0, 48000.0, false);
        pump (400);
        const double subBefore = q.getBandMeter().out[1].load(), midBefore = q.getBandMeter().out[3].load();
        lastStatus = juce::var();
        auto fin = cdlink::Endpoint::makeMessage ("analyze-finish");
        fin->setProperty ("seq", 7);
        master.send (fin);
        for (int i = 0; i < 40 && (int) lastStatus.getProperty ("extra", {}).getProperty ("finished", 0) != 7; ++i) pump (25);
        const auto ex2 = lastStatus.getProperty ("extra", {});
        std::cout << "       after finish: " << ex2.getProperty ("cleanup", "").toString() << std::endl;
        check ((int) ex2.getProperty ("finished", 0) == 7 && ! (bool) ex2.getProperty ("listening", true) && ex2.getProperty ("cleanup", "").toString().contains ("HIGH-PASS"),
               "PRESCRIBE: The Servant finishes, decides its clean-up and tells The Master right away");
        const double subRatio = 10.0 * std::log10 (q.getBandMeter().out[1].load() / subBefore), midRatio = 10.0 * std::log10 (q.getBandMeter().out[3].load() / midBefore);
        check (q.apvts.getRawParameterValue ("hpf")->load() > 150.0f && subRatio < -6.0 && std::abs (midRatio) < 1.0,
               juce::String::formatted ("what The Master reads is rescaled to the new high-pass (%.0f Hz: 60-120 Hz %.1f dB, 250-500 Hz %.1f dB)",
                                        q.apvts.getRawParameterValue ("hpf")->load(), subRatio, midRatio));

        // LOCK: The Master leaves this track's tone EQ alone
        setP (q, "toneLock", 1.0f);
        lastApplied = juce::var();
        auto set2 = cdlink::Endpoint::makeMessage ("set");
        auto* prm2 = new juce::DynamicObject();
        prm2->setProperty ("t1g", 4.0); prm2->setProperty ("trim", 2.0);
        set2->setProperty ("params", juce::var (prm2));
        master.send (set2, q.getLink().getId());
        for (int i = 0; i < 40 && ! lastApplied.isObject(); ++i) pump (50);
        check (std::abs (q.apvts.getRawParameterValue ("t1g")->load() + 2.5f) < 0.02f && std::abs (q.apvts.getRawParameterValue ("trim")->load() - 2.0f) < 0.02f,
               "LOCK keeps the tone EQ; The Master still sets the level");
        setP (q, "toneLock", 0.0f);
    }

    //==== DE-ESSER: a "voice" (vowels + an "s" burst every second) and a distorted guitar
    {
        auto voice = [] (int64_t n, int)
        {
            const double t = (double) n / 48000.0, ph = std::fmod (t, 1.0);
            double v = 0; for (int h = 1; h <= 12; ++h) v += std::sin (juce::MathConstants<double>::twoPi * 180.0 * h * t) / h;
            v *= 0.12;
            uint32_t z = (uint32_t) n * 2654435761u; z ^= z >> 15; z *= 2246822519u; z ^= z >> 13;
            const double nz = (double) (z & 0xffff) / 32768.0 - 1.0;
            static double hp1 = 0, prev = 0;   // crude high-pass for the "s" noise
            hp1 = 0.25 * (hp1 + nz - prev); prev = nz;
            const bool s = ph > 0.6 && ph < 0.72;
            return (float) (s ? 0.02 * v + 0.35 * hp1 : v);
        };
        auto hiPow = [] (const Render& r, double from, double to, bool sOnly)
        {
            svt::Biquad hp; hp.highpass (48000.0, 6000.0, 0.7);
            double e = 0; int c = 0;
            for (size_t i = 0; i < r.l.size(); ++i)
            {
                const float y = hp.process (r.l[i], 0);
                const double t = (double) i / 48000.0 + from, ph = std::fmod (t, 1.0);
                const bool inS = ph > 0.62 && ph < 0.70, inV = ph > 0.1 && ph < 0.5;
                if (t >= from && t < to && (sOnly ? inS : inV)) { e += (double) y * y; ++c; }
            }
            return 10.0 * std::log10 (e / std::max (1, c) + 1e-20);
        };
        ServantProcessor p;
        for (auto* id : { "cleanOn", "resOn", "toneOn", "match" }) setP (p, id, 0);
        setP (p, "role", 0.0f);
        const auto dry = render (p, voice, 8.0, 0.0);
        float p50, p95, perMin, secs; p.getEngine().sib.read (p50, p95, perMin, secs);
        std::cout << "       voice sibilance: p50 " << p50 << " p95 " << p95 << " dB, " << perMin << "/min over " << secs << " s" << std::endl;
        check (p95 - p50 > 12.0f && perMin > 30.0f && perMin < 120.0f, "it measures how sibilant a voice is (the s sounds jump far above the vowels)");
        setP (p, "deessOn", 1); setP (p, "deess", 60.0f); setP (p, "deessHz", 5500.0f);
        const auto wet = render (p, voice, 8.0, 0.0);
        const double sCut = hiPow (dry, 0, 8, true) - hiPow (wet, 0, 8, true), vCut = hiPow (dry, 0, 8, false) - hiPow (wet, 0, 8, false);
        check (sCut > 4.0 && std::abs (vCut) < 1.0, juce::String::formatted ("the de-esser turns the s sounds down %.1f dB and leaves the vowels alone (%.2f dB)", sCut, vCut));
        // low end untouched during an "s" (split band, not a whole-signal duck)
        setP (p, "deessOn", 0);
        auto lowPow = [] (const Render& r)
        {
            svt::Biquad lp, lp2; lp.lowpass (48000.0, 1500.0, 0.7071); lp2.lowpass (48000.0, 1500.0, 0.7071);
            double e = 0; int c = 0;
            for (size_t i = 0; i < r.l.size(); ++i) { const float y = lp2.process (lp.process (r.l[i], 0), 0); const double ph = std::fmod ((double) i / 48000.0, 1.0); if (ph > 0.62 && ph < 0.70) { e += (double) y * y; ++c; } }
            return 10.0 * std::log10 (e / std::max (1, c) + 1e-20);
        };
        check (std::abs (lowPow (dry) - lowPow (wet)) < 1.0, juce::String::formatted ("only the top is turned down: below 1.5 kHz moves %.2f dB", lowPow (dry) - lowPow (wet)));

        ServantProcessor g;
        for (auto* id : { "cleanOn", "resOn", "toneOn", "match" }) setP (g, id, 0);
        Riff rf (48000.0, false); Gen gn = [&rf] (int64_t n, int c) { return rf.sample (n, c); };
        render (g, gn, 8.0, 0.0);
        g.getEngine().sib.read (p50, p95, perMin, secs);
        std::cout << "       guitar sibilance: p50 " << p50 << " p95 " << p95 << " dB, " << perMin << "/min" << std::endl;
        check (p95 - p50 < 8.0f || p95 < -9.0f, "a distorted guitar doesn't read as sibilant");
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
