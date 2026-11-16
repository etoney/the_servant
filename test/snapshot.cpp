// Headless checks (run locally and in CI):
//  1) profiles: the built-in starter loads, bad JSON is rejected; a folder of songs becomes a
//     profile (one song is enough), measurements are cached, adding a song refines it,
//     empty folders are listed but can't be used, NEW makes a folder
//  2) analysis: a profile made from a clean riff, then the same riff made boxy and ringing: it
//     finds the planted resonances, cuts the boxy low-mids, and the result lands near the clean one
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

static void writeWav (const juce::File& f, Riff riff, double seconds)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat wav;
    auto os = f.createOutputStream();
    std::unique_ptr<juce::OutputStream> stream (os.release());
    auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (riff.sr).withNumChannels (2).withBitsPerSample (24));
    juce::AudioBuffer<float> b (2, (int) (seconds * riff.sr));
    for (int i = 0; i < b.getNumSamples(); ++i) for (int c = 0; c < 2; ++c) b.setSample (c, i, riff.sample (i, c));
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

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

    // profiles live in a scratch folder for the test
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("servant-test-profiles");
    root.deleteRecursively();
    svt::ProfileFolders::rootOverride() = root;
    const auto parts = root.getChildFile ("Parts");

    //==== 1) profiles
    {
        ServantProcessor p;
        const auto* prof = p.getLibrary().find ("Starter: Metal Rhythm Guitar");
        check (prof != nullptr && prof->usable() && prof->builtIn, "built-in starter profile loads");
        check (p.getProfileName() == "Starter: Metal Rhythm Guitar", "with no folders, the starter is selected");
        if (prof != nullptr)
        {
            double mean = 0; int n = 0;
            for (int k = 0; k < svt::kBands; ++k) if (svt::bandHz (k) >= 99 && svt::bandHz (k) <= 8100) { mean += prof->target[k]; ++n; }
            check (std::abs (mean / n) < 0.01, "profile target is normalised");
            check (prof->hpf == 80.0f && prof->lpf == 12000.0f, "profile HPF / LPF read");
        }
        svt::Profile bad;
        check (! svt::Profile::fromString ("{\"format\":\"nope\"}", bad), "a non-profile JSON is rejected");
        check (! svt::Profile::fromString ("{\"format\":\"castle-danger-profile\",\"name\":\"X\",\"curve\":[[100,0],[50,0]]}", bad),
               "a profile with frequencies out of order is rejected");
        check (parts.isDirectory() && root.getChildFile ("Mixes").isDirectory() && root.getChildFile ("READ ME.txt").existsAsFile(),
               "first run makes Profiles/Parts, Profiles/Mixes and a READ ME");
    }

    ServantProcessor proc;
    setP (proc, "match", 0);
    {
        writeWav (parts.getChildFile ("Clean Guitar").getChildFile ("song one.wav"), Riff (sr, false), 10.0);
        root.getChildFile ("Parts").getChildFile ("Empty One").createDirectory();
        proc.rescanProfilesNow();
        const auto* cg = proc.getLibrary().find ("Clean Guitar");
        check (cg != nullptr && cg->songs == 1 && cg->usable(), "a folder with one song becomes a profile");
        check (cg != nullptr && cg->tol[30] == 2.5f, "one song: fixed +-2.5 dB allowance");
        check (parts.getChildFile ("Clean Guitar").getChildFile (".castle-danger-cache.json").existsAsFile(), "measurements are cached in the folder");
        const auto* em = proc.getLibrary().find ("Empty One");
        check (em != nullptr && em->songs == 0 && ! em->usable(), "an empty folder is listed but can't be used yet");

        // a different processor (another plugin instance) reads the cache instead of measuring again
        ServantProcessor other;
        other.rescanProfilesNow();
        check (other.getLibrary().find ("Clean Guitar") != nullptr && other.getLibrary().find ("Clean Guitar")->songs == 1, "another instance sees the profile");

        writeWav (parts.getChildFile ("Clean Guitar").getChildFile ("song two.wav"), Riff (sr, false, 1.12), 10.0);
        proc.rescanProfilesNow();
        cg = proc.getLibrary().find ("Clean Guitar");
        check (cg != nullptr && cg->songs == 2, "dropping in another song refines the profile");

        check (proc.createProfileFolder ("Sabbath / Rhythm") && ! proc.getProfileName().containsChar ('/')
                   && parts.getChildFile (proc.getProfileName()).isDirectory() && proc.currentProfile() == nullptr,
               "NEW makes a folder with a safe name and selects it (empty until songs arrive)");
        proc.selectProfile ("Clean Guitar");
        check (proc.getProfileName() == "Clean Guitar" && proc.currentProfile() != nullptr, "picking a profile");
    }

    //==== 2) analysis on the test riff
    Riff riff (sr);
    Gen riffGen = [&riff] (int64_t n, int c) { return riff.sample (n, c); };
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
    check (a.excessAfter < a.excessBefore * 0.65f, "tone EQ alone takes out over a third of the error (the rings are left to the suppressor)");
    bool knobsMatch = true;
    for (int i = 0; i < a.nTone; ++i)
        knobsMatch = knobsMatch && std::abs (getP (proc, ServantProcessor::toneId ("g", i)) - a.tone[i].g) < 0.02f
                                && std::abs (getP (proc, ServantProcessor::toneId ("f", i)) / a.tone[i].f - 1.0f) < 0.01f;
    check (knobsMatch, "the decisions land on the knobs");
    check (std::abs (getP (proc, "lpf") - proc.currentProfile()->lpf) < 1.0f && std::abs (getP (proc, "hpf") - proc.currentProfile()->hpf) < 0.5f,
           "clean-up takes the profile's filters (" + juce::String (getP (proc, "hpf"), 0) + " Hz / " + juce::String (getP (proc, "lpf"), 0) + " Hz)");
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
    check (svt::rmsOf (ea) < svt::rmsOf (eb) * 0.6f, "processed audio is much closer to the profile");
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
        check (std::abs (q.getAnalysis().excessBefore - a.excessBefore) < 0.01f, "the saved analysis matches");
        check (q.getProfileName() == "Clean Guitar", "the profile choice comes back");

        // the session still opens right if the profile's folder is gone
        parts.getChildFile ("Clean Guitar").deleteRecursively();
        ServantProcessor r;
        r.rescanProfilesNow();
        r.setStateInformation (mb.getData(), (int) mb.getSize());
        check (r.getProfileName() == "Clean Guitar" && r.currentProfile() != nullptr
                   && std::abs (r.currentProfile()->target[30] - proc.currentProfile()->target[30]) < 0.01f,
               "a session keeps its profile even if the folder is deleted");
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
        check (q.getBandMeter().outSeconds.load() < 0.5, "The Master can restart the track measurement (in step with its own analysis)");
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

    root.deleteRecursively();
    std::cout << "\n" << failures << " failure(s)" << std::endl;
    return failures == 0 ? 0 : 1;
}
