#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ServantDsp.h"
#include "Profiles.h"

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

    //---- profiles (message thread): built-in starter + a folder of songs per profile in Profiles/Parts
    const svt::ProfileLibrary& getLibrary() const { return library; }
    void refreshProfiles();
    void rescanProfilesNow();            // measure new songs right away (the folders are also watched in the background)
    juce::String getProfileName() const { return profileName; }
    void selectProfile (const juce::String& name);
    const svt::Profile* currentProfile() const { auto* p = library.find (profileName); return p != nullptr && p->usable() ? p : nullptr; }
    juce::String getMeasuring() const { return folders.getMeasuring(); }
    bool createProfileFolder (const juce::String& name);   // makes Profiles/Parts/<name> and selects it

    //---- analysis (message thread)
    enum class Stage { idle, listening, done };
    Stage getStage() const;
    void startAnalysis();
    void finishAnalysis();               // stop listening, decide, apply (also called when playback stops)
    const svt::Analysis& getAnalysis() const { return analysis; }
    const svt::ResBand* getAppliedRes (int& n) const { n = nApplied; return applied; }
    std::atomic<int> analysisVersion { 0 };

    // knobs the user has moved since the last analysis show gold
    enum { kHpfFlag = svt::kTone, kLpfFlag, kNumFlags };
    bool isUserSet (int i) const { return userSet[(size_t) i].load(); }

    static juce::String toneId (const char* what, int band) { return "t" + juce::String (band + 1) + what; }

private:
    void parameterChanged (const juce::String& id, float) override;
    void timerCallback() override;
    void decideAndApply (bool applyToKnobs);
    void setParam (const juce::String& id, float value);

    svt::Engine engine;
    svt::ProfileFolders folders;
    svt::ProfileLibrary library;
    std::vector<svt::Profile> builtIns;
    svt::Profile sessionProfile;          // the selected profile as saved in the session (used if its folder is gone)
    bool hasSessionProfile = false;
    juce::String profileName, appliedSignature;
    int foldersVersionSeen = -1;

    std::vector<float> ltas, ltasEnv;   // what the analyzer heard (average power per FFT bin + its envelope)
    int ltasFrames = 0;
    double ltasRate = 48000.0;
    svt::Analysis analysis;
    svt::ResBand applied[svt::kMaxRes];
    int nApplied = 0;

    std::array<std::atomic<bool>, kNumFlags> userSet {};
    std::atomic<bool> applying { false }, finishRequested { false };
    bool sawPlaying = false;
    int reportedLatency = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ServantProcessor)
};
