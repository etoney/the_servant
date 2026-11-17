#include "PluginProcessor.h"
#include "PluginEditor.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

static const float kDefaultToneHz[svt::kTone] = { 100.0f, 250.0f, 630.0f, 1600.0f, 4000.0f, 10000.0f };

ServantProcessor::ServantProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Servant", createLayout())
{
    for (int i = 0; i < svt::kTone; ++i)
        for (auto* w : { "f", "g", "q" }) apvts.addParameterListener (toneId (w, i), this);
    for (auto* id : { "hpf", "lpf", "role", "deessHz" }) apvts.addParameterListener (id, this);

    juce::StringArray linkIds { "amount", "toneOn", "compOn", "comp", "trim", "role", "match", "hpf", "lpf", "deessOn", "deess", "deessHz", "transOn", "transAtk", "transSus" };
    for (int i = 0; i < svt::kTone; ++i) for (auto* w : { "f", "g", "q" }) linkIds.add (toneId (w, i));
    link = std::make_unique<Link> ("servant", "The Servant", JucePlugin_VersionString, apvts, linkIds);
    link->onBeforeSet = [this] { applying = true; };          // The Master's settings aren't "your" overrides
    link->onSetByMaster = [this] (const juce::StringArray& changed)
    {
        applying = false;
        for (int i = 0; i < svt::kTone; ++i)                    // its tone moves are the new starting point
            if (changed.contains (toneId ("g", i)) || changed.contains (toneId ("f", i))) userSet[(size_t) i] = false;
        ++analysisVersion;
    };
    link->filterSet = [this] (juce::DynamicObject& prm)        // LOCK: keep this track's tone EQ as it is
    {
        if (apvts.getRawParameterValue ("toneLock")->load() < 0.5f) return;
        prm.removeProperty ("toneOn");
        for (int i = 0; i < svt::kTone; ++i) for (auto* w : { "f", "g", "q" }) prm.removeProperty (toneId (w, i));
    };
    link->onOtherMessage = [this] (const juce::var& m)
    {
        const auto type = m.getProperty ("type", "").toString();
        if (type == "measure-reset") { bandMeter.requestReset(); engine.sib.resetRequested = true; engine.trans.resetRequested = true; startAnalysis(); }      // The Master's RESET: listen again
        else if (type == "analyze-finish")                                                // its PRESCRIBE: decide now
        {
            finishAnalysis();
            finishedSeq = juce::jmax (finishedSeq, (int) m.getProperty ("seq", 0));
            link->sendStatusNow();
        }
    };
    link->extraStatus = [this]
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("track", getTrackName());
        o->setProperty ("role", getRoleName());
        juce::Array<juce::var> b;
        for (auto& v : bandMeter.out) b.add (v.load());
        o->setProperty ("bandPow", b);
        o->setProperty ("pow", bandMeter.outTotal.load());
        o->setProperty ("peak", (double) bandMeter.outPeak.load());
        o->setProperty ("seconds", bandMeter.outSeconds.load());
        float p50, p95, perMin, secs; engine.sib.read (p50, p95, perMin, secs);
        auto* so = new juce::DynamicObject();
        so->setProperty ("p50", p50); so->setProperty ("p95", p95); so->setProperty ("perMin", perMin); so->setProperty ("seconds", secs);
        so->setProperty ("hz", apvts.getRawParameterValue ("deessHz")->load());
        o->setProperty ("sib", juce::var (so));
        float tp95, tsecs; engine.trans.read (tp95, tsecs);
        auto* to = new juce::DynamicObject();
        to->setProperty ("p95", tp95); to->setProperty ("seconds", tsecs);
        o->setProperty ("trans", juce::var (to));
        o->setProperty ("listening", getStage() == Stage::listening);
        o->setProperty ("finished", finishedSeq);
        int nRes = 0; getAppliedRes (nRes);
        o->setProperty ("cleanup", analysis.valid ? cleanup.why + "; " + juce::String (nRes) + (nRes == 1 ? " resonance" : " resonances") : juce::String());
        return juce::var (o);
    };
    startAnalysis();     // listens from the start, like The Master
    startTimerHz (10);
}

ServantProcessor::~ServantProcessor()
{
    stopTimer();
    for (int i = 0; i < svt::kTone; ++i)
        for (auto* w : { "f", "g", "q" }) apvts.removeParameterListener (toneId (w, i), this);
    for (auto* id : { "hpf", "lpf", "role", "deessHz" }) apvts.removeParameterListener (id, this);
}

juce::AudioProcessorValueTreeState::ParameterLayout ServantProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto dbAttr = AudioParameterFloatAttributes().withLabel ("dB")
        .withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; });
    auto pctAttr = AudioParameterFloatAttributes().withLabel ("%")
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + "%"; });
    auto hzAttr = AudioParameterFloatAttributes().withLabel ("Hz")
        .withStringFromValueFunction ([] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : String (roundToInt (v)) + " Hz"; });
    auto lpfAttr = AudioParameterFloatAttributes().withLabel ("Hz")
        .withStringFromValueFunction ([] (float v, int) { return v >= 19900.0f ? String ("OFF") : String (v / 1000.0f, 1) + " kHz"; });
    auto qAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return "Q " + String (v, 2); });

    auto skewed = [] (float lo, float hi, float centre) { NormalisableRange<float> r (lo, hi, 0.01f); r.setSkewForCentre (centre); return r; };

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Amount", NormalisableRange<float> (0.0f, 150.0f, 0.1f), 100.0f, pctAttr));

    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "cleanOn", 1 }, "Clean-up On", true));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "hpf", 1 }, "High-Pass", skewed (20.0f, 400.0f, 90.0f), 80.0f, hzAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "lpf", 1 }, "Low-Pass", skewed (2000.0f, 20000.0f, 9000.0f), 20000.0f, lpfAttr));

    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "resOn", 1 }, "Resonance On", true));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "resLock", 1 }, "Resonance Lock", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "resDepth", 1 }, "Resonance Depth", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 60.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "resSharp", 1 }, "Resonance Sharpness", skewed (2.0f, 16.0f, 8.0f), 10.0f, qAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "resSpeed", 1 }, "Resonance Speed", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, pctAttr));

    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "toneOn", 1 }, "Tone EQ On", true));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "toneLock", 1 }, "Tone EQ Lock", false));
    for (int i = 0; i < svt::kTone; ++i)
    {
        const String n = "Tone " + String (i + 1) + " ";
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { toneId ("f", i), 1 }, n + "Freq", skewed (20.0f, 20000.0f, 1000.0f), kDefaultToneHz[i], hzAttr));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { toneId ("g", i), 1 }, n + "Gain", NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, dbAttr));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { toneId ("q", i), 1 }, n + "Q", skewed (0.3f, 6.0f, 1.0f), 1.0f, qAttr));
    }

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "match", 1 }, "Match (Level-Matched Output)", true));
    // mix link (The Master sets these; you can too)
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "role", 1 }, "Role", roleNames(), 7));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "compOn", 1 }, "Compressor On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "comp", 1 }, "Compressor", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "trim", 1 }, "Trim", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "deessOn", 1 }, "De-Esser On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "deess", 1 }, "De-Esser", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 40.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "deessHz", 1 }, "De-Esser Frequency", skewed (3000.0f, 10000.0f, 6000.0f), 6500.0f, hzAttr));
    auto bipAttr = AudioParameterFloatAttributes().withLabel ("%")
        .withStringFromValueFunction ([] (float v, int) { const int i = roundToInt (v); return (i > 0 ? "+" : "") + String (i) + "%"; });
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "transOn", 1 }, "Transients On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "transAtk", 1 }, "Transient Attack", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, bipAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "transSus", 1 }, "Transient Sustain", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, bipAttr));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "compare", 1 }, "Compare (Hear the Dry Track)", false));
    return { p.begin(), p.end() };
}

svt::Params ServantProcessor::currentParams() const
{
    auto v = [this] (const juce::String& id) { return apvts.getRawParameterValue (id)->load(); };
    svt::Params p;
    p.amount  = v ("amount") * 0.01f;
    p.cleanOn = v ("cleanOn") > 0.5f;
    p.hpf = v ("hpf"); p.lpf = v ("lpf");
    p.resOn = v ("resOn") > 0.5f;
    p.resDepth = v ("resDepth") * 0.01f;
    p.resQ = v ("resSharp");
    p.resSpeed = v ("resSpeed") * 0.01f;
    p.resTol = svt::roleRules ((int) v ("role")).resTol;
    p.toneOn = v ("toneOn") > 0.5f;
    for (int i = 0; i < svt::kTone; ++i) p.tone[i] = { v (toneId ("f", i)), v (toneId ("g", i)), v (toneId ("q", i)) };
    p.match = v ("match") > 0.5f;
    p.compare = v ("compare") > 0.5f;
    p.compOn = v ("compOn") > 0.5f;
    p.comp = v ("comp") * 0.01f;
    p.trimDb = v ("trim");
    p.deessOn = v ("deessOn") > 0.5f;
    p.deess = v ("deess") * 0.01f;
    p.deessHz = v ("deessHz");
    p.transOn = v ("transOn") > 0.5f;
    p.transAtk = v ("transAtk") * 0.01f;
    p.transSus = v ("transSus") * 0.01f;
    return p;
}

bool ServantProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

void ServantProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    bandMeter.setRate (sampleRate);
    bandMeter.prepare (sampleRate);
    // keep what was heard if the host re-prepares at the same rate (it does that often)
    engine.prepare (sampleRate, samplesPerBlock);
    engine.setResonances (applied, nApplied);
    if (engine.getLatency() != reportedLatency) { reportedLatency = engine.getLatency(); setLatencySamples (reportedLatency); }
}

void ServantProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c) buffer.clear (c, 0, buffer.getNumSamples());

    engine.process (buffer, currentParams());
    if (buffer.getNumChannels() > 0)
    {
        const float* L = buffer.getReadPointer (0);
        const float* R = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr;
        bandMeter.process (L, R, buffer.getNumSamples());
        double pw = 0;
        for (int i = 0; i < buffer.getNumSamples(); ++i) pw += 0.5 * ((double) L[i] * L[i] + (R ? (double) R[i] * R[i] : (double) L[i] * L[i]));
        pw /= juce::jmax (1, buffer.getNumSamples());
        link->env.addPowers (pw, pw, buffer.getNumSamples());   // a track: what matters is what it sends on
    }
}

const juce::StringArray& ServantProcessor::roleNames()
{
    static const juce::StringArray r { "Lead", "Rhythm", "Bass", "Kick", "Snare", "Drums", "Keys / Pads", "Other" };
    return r;
}

juce::String ServantProcessor::getRoleName() const { return roleNames()[(int) apvts.getRawParameterValue ("role")->load()]; }

//==============================================================================
ServantProcessor::Stage ServantProcessor::getStage() const
{
    if (engine.analyzer.learning.load()) return Stage::listening;
    return analysis.valid ? Stage::done : Stage::idle;
}

void ServantProcessor::startAnalysis()
{
    engine.analyzer.start();
    ++analysisVersion;
}

void ServantProcessor::finishAnalysis()
{
    if (! engine.analyzer.learning.load()) return;
    engine.analyzer.stop();
    std::vector<float> heard, heardEnv;
    const int frames = engine.analyzer.getAverage (heard, heardEnv);
    if (frames >= 4)   // under ~0.7 s of real audio: keep the previous result
    {
        ltas = std::move (heard);
        ltasEnv = std::move (heardEnv);
        ltasFrames = frames;
        ltasRate = engine.analyzer.getSampleRate();
        decideAndApply (true);
    }
    ++analysisVersion;
}

void ServantProcessor::setParam (const juce::String& id, float value)
{
    if (auto* rp = apvts.getParameter (id))
    {
        rp->beginChangeGesture();
        rp->setValueNotifyingHost (rp->convertTo0to1 (value));
        rp->endChangeGesture();
    }
}

svt::RoleProfile ServantProcessor::roleProfile() const
{
    svt::RoleProfile p;
    p.hpf = cleanup.hpf; p.lpf = cleanup.lpf; p.resTol = cleanup.resTol;
    return p;
}

void ServantProcessor::decideAndApply (bool applyToKnobs)
{
    if (ltasFrames <= 0) return;
    // the clean-up from where this track's content really starts and ends, plus its ROLE...
    float measured[svt::kBands];
    svt::bandCurve (ltas, ltasRate, measured);
    cleanup = svt::decideCleanup (measured, (int) apvts.getRawParameterValue ("role")->load());
    // ...and the resonances inside that range (no tone moves: The Master decides those from the mix)
    analysis = svt::decide (ltas, ltasEnv, ltasRate, ltasFrames, roleProfile());
    if (! applyToKnobs) return;

    const float hpfOld = apvts.getRawParameterValue ("hpf")->load(), lpfOld = apvts.getRawParameterValue ("lpf")->load();
    applying = true;
    const bool locked = apvts.getRawParameterValue ("toneLock")->load() > 0.5f;
    if (! locked && ! userSet[kHpfFlag].load()) setParam ("hpf", cleanup.hpf);
    if (! locked && ! userSet[kLpfFlag].load()) setParam ("lpf", cleanup.lpf);
    // the de-esser's split: just under where this track's "s" energy peaks (4.5 - 9.5 kHz)
    if (! userSet[kDeessHzFlag].load())
    {
        int kBest = -1;
        for (int k = 0; k < svt::kBands; ++k)
            if (svt::bandHz (k) >= 4500.0f && svt::bandHz (k) <= 9500.0f && (kBest < 0 || measured[k] > measured[kBest])) kBest = k;
        if (kBest >= 0) { deessHzDecided = juce::jlimit (3500.0f, 8000.0f, std::round (svt::bandHz (kBest) * 0.75f / 100.0f) * 100.0f); setParam ("deessHz", deessHzDecided); }
    }
    if (apvts.getRawParameterValue ("resLock")->load() < 0.5f)
    {
        nApplied = analysis.nRes;
        for (int i = 0; i < nApplied; ++i) applied[i] = analysis.res[i];
        for (auto& m : engine.muted) m = false;
        engine.setResonances (applied, nApplied);
    }
    applying = false;
    // what The Master reads from this track now matches the new filters
    const float hpfNew = apvts.getRawParameterValue ("hpf")->load(), lpfNew = apvts.getRawParameterValue ("lpf")->load();
    if (apvts.getRawParameterValue ("cleanOn")->load() > 0.5f && (hpfNew != hpfOld || lpfNew != lpfOld))
        bandMeter.requestScale (svt::cleanupBandChange (ltasRate, hpfOld, lpfOld, hpfNew, lpfNew));
}

void ServantProcessor::parameterChanged (const juce::String& id, float)
{
    if (id == "role") { roleChanged = true; return; }   // re-decide the clean-up for the new role (timer)
    if (applying.load()) return;
    if (id == "hpf") userSet[kHpfFlag] = true;
    else if (id == "deessHz") userSet[kDeessHzFlag] = true;
    else if (id == "lpf") userSet[kLpfFlag] = true;
    else if (id.startsWith ("t")) userSet[(size_t) juce::jlimit (0, svt::kTone - 1, id.substring (1, 2).getIntValue() - 1)] = true;
}

void ServantProcessor::timerCallback()
{
    if (roleChanged.exchange (false) && ltasFrames > 0 && getStage() != Stage::listening) { decideAndApply (true); ++analysisVersion; }
}

//==============================================================================
void ServantProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::ValueTree s ("SERVANT");
    juce::String flags, res, mutes;
    for (auto& f : userSet) flags << (f.load() ? "1" : "0");
    for (int i = 0; i < nApplied; ++i)
    {
        res << juce::String (applied[i].f, 2) << ":" << juce::String (applied[i].prom, 2) << ":";
        for (int q = 0; q < svt::kRefQs; ++q) res << juce::String (applied[i].dRef[q], 3) << (q + 1 < svt::kRefQs ? "|" : "");
        res << ";";
    }
    for (int i = 0; i < svt::kMaxRes; ++i) mutes << (engine.muted[(size_t) i].load() ? "1" : "0");
    s.setProperty ("userSet", flags, nullptr);
    s.setProperty ("res", res, nullptr);
    s.setProperty ("muted", mutes, nullptr);
    if (ltasFrames > 0)
    {
        s.setProperty ("ltas", juce::MemoryBlock (ltas.data(), ltas.size() * sizeof (float)).toBase64Encoding(), nullptr);
        s.setProperty ("ltasEnv", juce::MemoryBlock (ltasEnv.data(), ltasEnv.size() * sizeof (float)).toBase64Encoding(), nullptr);
        s.setProperty ("ltasFrames", ltasFrames, nullptr);
        s.setProperty ("ltasRate", ltasRate, nullptr);
    }
    s.setProperty ("version", JucePlugin_VersionString, nullptr);
    state.removeChild (state.getChildWithName ("SERVANT"), nullptr);
    state.appendChild (s, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void ServantProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    auto s = state.getChildWithName ("SERVANT");
    state.removeChild (s, nullptr);

    applying = true;
    apvts.replaceState (state);
    applying = false;
    if (! s.isValid()) return;

    const juce::String flags = s.getProperty ("userSet", "");
    for (int i = 0; i < kNumFlags; ++i) userSet[(size_t) i] = flags.substring (i, i + 1) == "1";

    nApplied = 0;
    for (auto& tok : juce::StringArray::fromTokens (s.getProperty ("res", "").toString(), ";", ""))
    {
        auto parts = juce::StringArray::fromTokens (tok, ":", "");
        if (parts.size() < 3 || nApplied >= svt::kMaxRes) continue;
        svt::ResBand rb; rb.f = parts[0].getFloatValue(); rb.prom = parts[1].getFloatValue();
        auto refs = juce::StringArray::fromTokens (parts[2], "|", "");
        for (int q = 0; q < svt::kRefQs && q < refs.size(); ++q) rb.dRef[q] = refs[q].getFloatValue();
        applied[nApplied++] = rb;
    }
    engine.setResonances (applied, nApplied);
    const juce::String mutes = s.getProperty ("muted", "");
    for (int i = 0; i < svt::kMaxRes; ++i) engine.muted[(size_t) i] = mutes.substring (i, i + 1) == "1";

    juce::MemoryBlock mb, mbEnv;
    if (mb.fromBase64Encoding (s.getProperty ("ltas", "").toString()) && mb.getSize() >= sizeof (float) * (svt::kFftSize / 2)
        && mbEnv.fromBase64Encoding (s.getProperty ("ltasEnv", "").toString()) && mbEnv.getSize() == mb.getSize())
    {
        ltas.assign ((const float*) mb.getData(), (const float*) mb.getData() + mb.getSize() / sizeof (float));
        ltasEnv.assign ((const float*) mbEnv.getData(), (const float*) mbEnv.getData() + mbEnv.getSize() / sizeof (float));
        ltasFrames = s.getProperty ("ltasFrames", 0);
        ltasRate = s.getProperty ("ltasRate", 48000.0);
        decideAndApply (false);   // for the display only: the knobs come back exactly as saved
        analysis.nRes = nApplied;
        for (int i = 0; i < nApplied; ++i) analysis.res[i] = applied[i];
    }
    ++analysisVersion;
}

juce::AudioProcessorEditor* ServantProcessor::createEditor() { return new ServantEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ServantProcessor(); }
