#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

static const float kDefaultToneHz[svt::kTone] = { 100.0f, 250.0f, 630.0f, 1600.0f, 4000.0f, 10000.0f };

ServantProcessor::ServantProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Servant", createLayout())
{
    reloadProfiles();
    for (int i = 0; i < svt::kTone; ++i)
        for (auto* w : { "f", "g", "q" }) apvts.addParameterListener (toneId (w, i), this);
    apvts.addParameterListener ("hpf", this);
    apvts.addParameterListener ("lpf", this);
    startTimerHz (10);
}

ServantProcessor::~ServantProcessor()
{
    stopTimer();
    for (int i = 0; i < svt::kTone; ++i)
        for (auto* w : { "f", "g", "q" }) apvts.removeParameterListener (toneId (w, i), this);
    apvts.removeParameterListener ("hpf", this);
    apvts.removeParameterListener ("lpf", this);
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
    if (auto* prof = currentProfile()) p.resTol = prof->resTol;
    p.toneOn = v ("toneOn") > 0.5f;
    for (int i = 0; i < svt::kTone; ++i) p.tone[i] = { v (toneId ("f", i)), v (toneId ("g", i)), v (toneId ("q", i)) };
    p.match = v ("match") > 0.5f;
    p.compare = v ("compare") > 0.5f;
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
    // keep what was heard if the host re-prepares at the same rate (it does that often)
    engine.prepare (sampleRate, samplesPerBlock);
    engine.setResonances (applied, nApplied);
    if (engine.getLatency() != reportedLatency) { reportedLatency = engine.getLatency(); setLatencySamples (reportedLatency); }
}

void ServantProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c) buffer.clear (c, 0, buffer.getNumSamples());

    // listening finishes by itself when the song stops playing
    if (engine.analyzer.learning.load())
    {
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (pos->getIsPlaying()) sawPlaying = true;
                else if (sawPlaying && engine.analyzer.learnedSeconds() > 3.0f) { sawPlaying = false; finishRequested = true; }
            }
    }
    engine.process (buffer, currentParams());
}

//==============================================================================
void ServantProcessor::reloadProfiles()
{
    juce::StringArray builtIn;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        int size = 0;
        if (auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
            builtIn.add (juce::String::fromUTF8 (data, size));
    }
    library.load (builtIn);
    if (library.find (genre, source) == nullptr && ! library.all().empty())
    {
        genre = library.all().front().genre;
        source = library.all().front().source;
    }
}

void ServantProcessor::selectProfile (const juce::String& g, const juce::String& s)
{
    if (library.find (g, s) == nullptr) return;
    genre = g; source = s;
    if (ltasFrames > 0 && ! engine.analyzer.learning.load()) decideAndApply (true);   // re-decide from what was heard, no need to play again
    ++analysisVersion;
}

ServantProcessor::Stage ServantProcessor::getStage() const
{
    if (engine.analyzer.learning.load()) return Stage::listening;
    return analysis.valid ? Stage::done : Stage::idle;
}

void ServantProcessor::startAnalysis()
{
    sawPlaying = false;
    finishRequested = false;
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

void ServantProcessor::decideAndApply (bool applyToKnobs)
{
    const auto* prof = currentProfile();
    if (prof == nullptr || ltasFrames <= 0) return;
    analysis = svt::decide (ltas, ltasEnv, ltasRate, ltasFrames, *prof);
    if (! applyToKnobs) return;

    applying = true;
    if (apvts.getRawParameterValue ("toneLock")->load() < 0.5f)
    {
        for (int i = 0; i < svt::kTone; ++i)
        {
            const bool used = i < analysis.nTone;
            setParam (toneId ("f", i), used ? analysis.tone[i].f : kDefaultToneHz[i]);
            setParam (toneId ("g", i), used ? analysis.tone[i].g : 0.0f);
            setParam (toneId ("q", i), used ? analysis.tone[i].q : 1.0f);
        }
        setParam ("hpf", prof->hpf);
        setParam ("lpf", prof->lpf);
        for (auto& f : userSet) f = false;
    }
    if (apvts.getRawParameterValue ("resLock")->load() < 0.5f)
    {
        nApplied = analysis.nRes;
        for (int i = 0; i < nApplied; ++i) applied[i] = analysis.res[i];
        for (auto& m : engine.muted) m = false;
        engine.setResonances (applied, nApplied);
    }
    applying = false;
}

void ServantProcessor::parameterChanged (const juce::String& id, float)
{
    if (applying.load()) return;
    if (id == "hpf") userSet[kHpfFlag] = true;
    else if (id == "lpf") userSet[kLpfFlag] = true;
    else if (id.startsWith ("t")) userSet[(size_t) juce::jlimit (0, svt::kTone - 1, id.substring (1, 2).getIntValue() - 1)] = true;
}

void ServantProcessor::timerCallback()
{
    if (finishRequested.exchange (false)) finishAnalysis();
}

//==============================================================================
void ServantProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::ValueTree s ("SERVANT");
    s.setProperty ("genre", genre, nullptr);
    s.setProperty ("source", source, nullptr);
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

    const juce::String g = s.getProperty ("genre", genre), src = s.getProperty ("source", source);
    if (library.find (g, src) != nullptr) { genre = g; source = src; }
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
