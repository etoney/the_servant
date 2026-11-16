#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class ServantLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ServantLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int h) override;
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool hi, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool hi, bool down) override;
    juce::Image rustDisc;
};

// The service bell: rings (glows) while The Servant is listening
class BellIcon : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    float glow = 0.0f;
};

// The big order plate: what The Servant is doing for The Master (LISTENING / SERVED / WAITING).
// Not a button any more: The Master starts and finishes the listening.
class AnalyzeButton : public juce::Button
{
public:
    AnalyzeButton() : juce::Button ("orders") { setInterceptsMouseClicks (false, false); }
    void paintButton (juce::Graphics&, bool hi, bool down) override;
    bool listening = false;
    float pulse = 0.0f;
    juce::String sub;
};

// What The Servant is doing, on a dark screen
class StatusScreen : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    juce::StringArray lines;
    juce::Array<juce::Colour> colours;
};

// The curve: the track, the result, the tone-EQ nodes (drag them) and the
// resonances (click to mute one)
class CurveDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CurveDisplay (ServantProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override { hoverNode = hoverRes = -1; repaint(); }

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (30.0f, 14.0f).withTrimmedTop (12.0f).withTrimmedBottom (6.0f); }
    float xFor (float hz) const;
    float hzFor (float x) const;
    float yFor (float db) const;
    float dbFor (float y) const;
    int nodeAt (juce::Point<float>) const;
    int resAt (juce::Point<float>) const;
    float eqDb (float hz, const svt::Params&) const;
    void setParam (const juce::String& id, float v);

    ServantProcessor& proc;
    int hoverNode = -1, hoverRes = -1, dragNode = -1;
};

class ServantEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ServantEditor (ServantProcessor&);
    ~ServantEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    struct Plate { juce::String title; juce::Rectangle<int> bounds; bool hot = false; };

    Knob& makeKnob (const juce::String& id, const juce::String& text, const juce::String& tip, float labelSize = 13.0f);
    void placeKnob (Knob&, juce::Rectangle<int>);
    void timerCallback() override;
    void buildBackdrop();
    void updateStatus();

    ServantProcessor& proc;
    ServantLookAndFeel lnf;
    std::vector<std::unique_ptr<Knob>> knobs;
    Knob *compKnob = nullptr, *trimKnob = nullptr, *deessKnob = nullptr, *deessHzKnob = nullptr;
    juce::ToggleButton deessOn { "ON" };
    juce::Label deessGr;
    float deessGrShown = 0.0f;
    juce::ComboBox roleBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> roleAtt;
    juce::ToggleButton compOn { "ON" };
    juce::Label linkLabel;
    int linkTick = 0;
    Knob *amountKnob = nullptr, *hpfKnob = nullptr, *lpfKnob = nullptr, *depthKnob = nullptr, *sharpKnob = nullptr, *speedKnob = nullptr;
    Knob* toneKnobs[svt::kTone] {};

    AnalyzeButton analyzeButton;
    StatusScreen status;
    CurveDisplay curve;
    BellIcon bell;
    juce::ToggleButton matchButton { "MATCH" }, compareButton { "COMPARE" },
                       cleanOn { "CLEAN-UP ON" }, resOn { "ON" }, resLock { "LOCK" }, toneOn { "ON" }, toneLock { "LOCK" };
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAtts;
    juce::TooltipWindow tooltips { this, 700 };

    std::vector<Plate> plates;
    juce::Rectangle<int> logoArea, badgeArea, crestArea, roleArea, stripeArea;
    juce::Image backdrop, logoImage;
    int lastVersion = -1;
    float phase = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ServantEditor)
};
