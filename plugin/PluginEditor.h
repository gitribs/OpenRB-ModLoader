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
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
};

class ModSlider final : public juce::Slider
{
public:
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
};

class ModTextButton final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
};

class ModToggleButton final : public juce::ToggleButton
{
public:
    using juce::ToggleButton::ToggleButton;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
};

class StepButton final : public juce::TextButton
{
public:
    void setCell (int value, bool is909);
    void setDownbeat (bool isDownbeat);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

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
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

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
    void updateMachineFocus (juce::Point<int> pos);
    void setupKnob (juce::Slider& s, bool red);
    void setupCaptionLabel (juce::Label&, const juce::String&);
    void setupPatternSection (int machine);
    void setup808();
    void setup909();
    void sync808Radios();
    void refreshSteps();
    void refreshModLabel();
    void refreshPatternHeader();
    void refreshShuffle();
    void refreshSplash();
    void refreshDisable();
    void setupDisable (juce::ToggleButton& btn, int machine);
    void click808Step (int step);
    void click909Step (int step);
    void select808 (int col);
    void select909 (int col);
    void bumpLength (int machine, int delta);
    void handleNestedMouseDown (const juce::MouseEvent&);
    void loadMod (int delta);
    void loadModIndex (int index);
    void toggleModPicker();
    void showModPicker();
    void hideModPicker();
    void chooseModsFolder();
    void applyPickedMod (const juce::File&);
    void chooseDat();
    void showContextMenu();
    void resetDevice();
    void editCut();
    void editCopy();
    void editPaste (bool replace);
    void editClear();
    void editShift (bool right, bool drumOnly);
    void editRandomize (bool drumOnly);
    void editAlter (bool drumOnly);
    bool loadFocusedPattern (uint8_t* dest);
    void storeFocusedPattern (const uint8_t* src);
    int focusedDrum() const;
    uint8_t currentPattern (int machine) const;
    OrbModloader* eng() { return proc.engine(); }

    ModloaderProcessor& proc;
    ModloaderLookAndFeel lnf;

    juce::Label title, modName, status, shuffleN, flamN;
    IncDecArrowButton modUp { true }, modDown { false };
    ModTextButton folderBtn { "MOD" }, datBtn { "DAT" };
    ModSlider shuffle;
    juce::AudioProcessorValueTreeState::SliderAttachment shuffleA;
    ModToggleButton shuffleOn;

    struct PatternUi
    {
        ModToggleButton enable { "ON" };
        ModTextButton bank[4];
        ModTextButton pat[8];
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

    std::array<ModSlider, ORB_MODLOADER_P808> k808;
    std::array<juce::Label, ORB_MODLOADER_P808> n808;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, ORB_MODLOADER_P808> k808A;
    std::array<ModToggleButton, 5> sw808;
    std::array<ModToggleButton, 5> sw808Def;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 5> sw808A;
    std::array<juce::Label, 12> legend808;
    std::array<StepButton, 16> step808;
    std::array<StepLed, 16> led808;
    std::array<juce::Rectangle<int>, 12> slot808;
    int sel808 = 1;

    std::array<ModSlider, ORB_MODLOADER_P909> k909;
    std::array<juce::Label, ORB_MODLOADER_P909> n909;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, ORB_MODLOADER_P909> k909A;
    std::array<juce::Label, 12> legend909;
    std::array<juce::Rectangle<int>, 12> slot909;
    std::array<StepButton, 16> step909;
    std::array<StepLed, 16> led909;
    ModToggleButton flamEntry;
    ModToggleButton disable808 { "MUTE" }, disable909 { "MUTE" };
    int sel909 = 1;
    int focusedMachine = -1;
    juce::Image splashImage;
    juce::Rectangle<int> splash808;
    int splashMod = -1;

    struct FocusListener final : public juce::MouseListener
    {
        explicit FocusListener (ModloaderEditor& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.handleNestedMouseDown (e);
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu())
                return;
            owner.updateMachineFocus (e.getEventRelativeTo (&owner).getPosition());
            owner.showContextMenu();
        }
        ModloaderEditor& owner;
    };
    FocusListener focusListener { *this };

    class ModPicker;
    std::unique_ptr<ModPicker> modPicker;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModloaderEditor)
};
