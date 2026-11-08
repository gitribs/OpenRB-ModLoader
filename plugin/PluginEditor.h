#pragma once

#include "PluginProcessor.h"

#include <array>
#include <memory>

class ModloaderLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ModloaderLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool isEnabled, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;
    juce::Button* createSliderButton (juce::Slider&, bool isIncrement) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
};

class StepButton final : public juce::TextButton
{
public:
    void setCell (int value, bool is909);
    void setDownbeat (bool isDownbeat);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    int value = 0;
    bool is909 = false;
    bool downbeat = false;
};

class StepLed final : public juce::Component
{
public:
    void setOn (bool shouldBeOn);
    void paint (juce::Graphics&) override;

private:
    bool on = false;
};

class IncDecArrowButton final : public juce::Button
{
public:
    explicit IncDecArrowButton (bool up);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    bool isUp;
};

class ModloaderEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit ModloaderEditor (ModloaderProcessor&);
    ~ModloaderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void setupKnob (juce::Slider& s, bool red);
    void setupCaptionLabel (juce::Label&, const juce::String&);
    void setupPatternSection (int machine);
    void setup808();
    void setup909();
    void sync808Radios();
    void refreshSteps();
    void refreshModLabel();
    void refreshPatternHeader();
    void refreshDisable();
    void setupDisable (juce::ToggleButton& btn, int machine);
    void click808Step (int step);
    void click909Step (int step);
    void select808 (int col);
    void select909 (int col);
    void bumpLength (int machine, int delta);
    void loadMod (int delta);
    void chooseModsFolder();
    void applyPickedMod (const juce::File&);
    void chooseDat();
    uint8_t currentPattern (int machine) const;
    OrbModloader* eng() { return proc.engine(); }

    ModloaderProcessor& proc;
    ModloaderLookAndFeel lnf;

    juce::Label title, modName, status, shuffleN, flamN;
    IncDecArrowButton modUp { true }, modDown { false };
    juce::TextButton folderBtn { "MOD" }, datBtn { "DAT" };
    juce::Slider shuffle;
    juce::AudioProcessorValueTreeState::SliderAttachment shuffleA;

    struct PatternUi
    {
        juce::ToggleButton enable { "ON" };
        juce::TextButton bank[4];
        juce::TextButton pat[8];
        juce::ToggleButton shuffleBtn { "SHUFFLE" };
        juce::Label stepsLabel { {}, "STEPS" };
        juce::Label stepsValue { {}, "16" };
        IncDecArrowButton stepsUp { true };
        IncDecArrowButton stepsDown { false };
        juce::AudioProcessorValueTreeState::ButtonAttachment enableA;
        int shownBank = 0;
        PatternUi (juce::AudioProcessorValueTreeState& apvts, const juce::String& enableId)
            : enableA (apvts, enableId, enable)
        {
        }
    };
    PatternUi pat808, pat909;

    std::array<juce::Slider, ORB_MODLOADER_P808> k808;
    std::array<juce::Label, ORB_MODLOADER_P808> n808;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, ORB_MODLOADER_P808> k808A;
    std::array<juce::ToggleButton, 5> sw808;
    std::array<juce::ToggleButton, 5> sw808Def;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 5> sw808A;
    std::array<juce::Label, 12> legend808;
    std::array<StepButton, 16> step808;
    std::array<StepLed, 16> led808;
    std::array<juce::Rectangle<int>, 12> slot808;
    int sel808 = 1;

    std::array<juce::Slider, ORB_MODLOADER_P909> k909;
    std::array<juce::Label, ORB_MODLOADER_P909> n909;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, ORB_MODLOADER_P909> k909A;
    std::array<juce::Label, 12> legend909;
    std::array<juce::Rectangle<int>, 12> slot909;
    std::array<StepButton, 16> step909;
    std::array<StepLed, 16> led909;
    juce::ToggleButton flamEntry;
    juce::ToggleButton disable808 { "MUTE" }, disable909 { "MUTE" };
    int sel909 = 1;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModloaderEditor)
};
