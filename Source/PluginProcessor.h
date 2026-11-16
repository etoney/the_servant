#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ServantDsp.h"
#include "Roles.h"
#include "CDLink.h"

class ServantProcessor : public juce::AudioProcessor,
                         private juce::AudioProcessorValueTreeState::Listener,
                         private juce::Timer
{
public:
    ServantProcessor();
    ~ServantProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "The Servant"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;
    svt::Params currentParams() const;
    svt::Engine& getEngine() { return engine; }

    //---- analysis (message thread). The Master runs it: its RESET starts every Servant listening,
    //    its PRESCRIBE makes each one finish and decide its clean-up and resonances.
    enum class Stage { idle, listening, done };
    Stage getStage() const;
    void startAnalysis();
    void finishAnalysis();               // stop listening, decide clean-up + resonances from this track and its ROLE, apply
    const svt::Cleanup& getCleanup() const { return cleanup; }
    const svt::Analysis& getAnalysis() const { return analysis; }
    const svt::ResBand* getAppliedRes (int& n) const { n = nApplied; return applied; }
    std::atomic<int> analysisVersion { 0 };

    // knobs the user has moved since the last analysis show gold
    enum { kHpfFlag = svt::kTone, kLpfFlag, kNumFlags };
    bool isUserSet (int i) const { return userSet[(size_t) i].load(); }

    static juce::String toneId (const char* what, int band) { return "t" + juce::String (band + 1) + what; }

    // ---- Castle Danger Link: The Master hears the whole mix and can set this track's
    //      tone EQ, compressor and trim; ROLE tells it what the track is for
    static const juce::StringArray& roleNames();
    juce::String getRoleName() const;
    juce::String getTrackName() const { const juce::ScopedLock sl (nameLock); return trackName; }
    void updateTrackProperties (const TrackProperties& props) override { const juce::ScopedLock sl (nameLock); trackName = props.name.value_or (juce::String()); }
    using Link = cdlink::PluginLink<juce::AudioProcessorValueTreeState>;
    Link& getLink() { return *link; }
    svt::BandMeter& getBandMeter() { return bandMeter; }

private:
    std::unique_ptr<Link> link;
    svt::BandMeter bandMeter;
    juce::String trackName;
    juce::CriticalSection nameLock;
    void parameterChanged (const juce::String& id, float) override;
    svt::RoleProfile roleProfile() const;
    svt::Cleanup cleanup;
    int finishedSeq = 0;
    std::atomic<bool> roleChanged { false };
    void timerCallback() override;
    void decideAndApply (bool applyToKnobs);
    void setParam (const juce::String& id, float value);

    svt::Engine engine;
    std::vector<float> ltas, ltasEnv;   // what the analyzer heard (average power per FFT bin + its envelope)
    int ltasFrames = 0;
    double ltasRate = 48000.0;
    svt::Analysis analysis;
    svt::ResBand applied[svt::kMaxRes];
    int nApplied = 0;

    std::array<std::atomic<bool>, kNumFlags> userSet {};
    std::atomic<bool> applying { false };
    int reportedLatency = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ServantProcessor)
};
