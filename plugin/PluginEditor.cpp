#include "PluginEditor.h"

namespace
{
const juce::Colour kBg { 0xff161618 };
const juce::Colour kPanel { 0xff242428 };
const juce::Colour kAmber { 0xffe0a020 };
const juce::Colour kText { 0xffe6e4dc };
const juce::Colour kMuted { 0xff8a8680 };
const juce::Colour kWell { 0xff1a1a1c };
const juce::Colour kLine { 0xff3a3a40 };
const juce::Colour kRed { 0xffc44c3a };
const juce::Colour kVelHigh { 0xffb03c32 };
const juce::Colour kGreen { 0xff3cb86a };
const juce::Colour kLed { 0xff6ec4f0 };
const juce::Colour kBeatWell { 0xff3a3a42 };
const juce::Colour kWhite { 0xffffffff };
const juce::Colour kSlotA { 0xff1f1f24 };
const juce::Colour kSlotB { 0xff2c2c32 };
constexpr int kEditorW = 1040;
constexpr int kEditorH = 640;
constexpr int kKnobSize = 20;
constexpr int kFlamKnob = 28;
constexpr int kLabelH = 14;
constexpr int kCtrlRow = kLabelH + kKnobSize + 4;
constexpr int kSlot0X = 148;
constexpr int kSlotAreaW = 876;
const juce::Rectangle<int> kMachine808 { 8, 64, kEditorW - 16, 270 };
const juce::Rectangle<int> kMachine909 { 8, 342, kEditorW - 16, 290 };
constexpr int k808SlotW = kSlotAreaW / 12;
constexpr int k909SlotW[] = { 56, 92, 92, 88, 88, 88, 56, 56, 56, 68, 68, 68 };
constexpr int kSwitch808[] = { 11, 14, 17, 19, 21 };
const char* k808Legend[] = { "AC", "BD", "SD", "LT", "MT", "HT", "RS", "CP", "CB", "CY", "OH", "CH" };
const char* k909Legend[] = { "AC", "BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CC", "RC" };
const char* kBanks[] = { "A", "B", "C", "D" };
const char* k808KnobLabel[ORB_MODLOADER_P808] = {
    nullptr, nullptr, "LEVEL", "LEVEL", "TONE", "DECAY", "LEVEL", "TONE", "SNAP", "LEVEL", "TUNE",
    nullptr, "LEVEL", "TUNE", nullptr, "LEVEL", "TUNE", nullptr, "LEVEL", nullptr, "LEVEL", nullptr,
    "LEVEL", "LEVEL", "TONE", "DECAY", "LEVEL", "DECAY", "LEVEL"
};
const char* k909KnobLabel[ORB_MODLOADER_P909] = {
    nullptr, nullptr, "LEVEL", "LEVEL", "TUNE", "ATT", "DEC", "LEVEL", "TUNE", "TONE", "SNAP",
    "LEVEL", "TUNE", "DEC", "LEVEL", "TUNE", "DEC", "LEVEL", "TUNE", "DEC", "LEVEL", "LEVEL",
    "LEVEL", "DEC", "DEC", "LEVEL", "TUNE", "LEVEL", "TUNE", nullptr
};

enum class SlotSide { Centre, Left, Right };

int slot909X (int col)
{
    int x = kSlot0X;
    for (int i = 0; i < col; ++i)
        x += k909SlotW[i];
    return x;
}

int switchIndex (int ctrl)
{
    for (int i = 0; i < 5; ++i)
        if (kSwitch808[i] == ctrl)
            return i;
    return -1;
}

bool is808Switch (int ctrl)
{
    return switchIndex (ctrl) >= 0;
}

bool is808Level (int ctrl)
{
    switch (ctrl)
    {
        case 3: case 6: case 9: case 12: case 15: case 18: case 20: case 22: case 23: case 26: case 28:
            return true;
        default:
            return false;
    }
}

int mixBusFor808 (int col)
{
    // AC has no aux. Panel order is CY, OH, CH; voice/bus order is OH, CH, CY.
    switch (col)
    {
        case 1:  return 0;  // BD
        case 2:  return 1;  // SD
        case 3:  return 2;  // LT
        case 4:  return 3;  // MT
        case 5:  return 4;  // HT
        case 6:  return 5;  // RS
        case 7:  return 6;  // CP
        case 8:  return 7;  // CB
        case 9:  return 10; // CY
        case 10: return 8;  // OH
        case 11: return 9;  // CH
        default: return -1;
    }
}

int mixBusFor909 (int col)
{
    if (col <= 0)
        return -1;
    return 11 + (col - 1);
}

int toggleContentWidth (const juce::ToggleButton& button, int height)
{
    const bool radio = (bool) button.getProperties()["radio"];
    const bool caption = (bool) button.getProperties()["caption"];
    const auto fontSize = juce::jmin (15.0f, (float) height * 0.75f);
    const auto tickWidth = radio ? juce::jmin (11.0f, (float) height - 2.0f)
                                 : fontSize * 1.1f;
    const auto font = caption ? juce::Font { juce::FontOptions().withHeight (11.0f).withStyle ("Bold") }
                              : juce::Font { juce::FontOptions (fontSize) };
    const int textW = juce::GlyphArrangement::getStringWidthInt (font, button.getButtonText());
    return juce::roundToInt (tickWidth) + textW + 16;
}
} // namespace

IncDecArrowButton::IncDecArrowButton (bool up) : juce::Button ({}), isUp (up) {}

void IncDecArrowButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (kWell);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (highlighted || down ? kAmber : kLine);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    juce::Path p;
    const auto c = r.getCentre();
    constexpr float w = 5.0f;
    constexpr float h = 3.6f;
    if (isUp)
        p.addTriangle (c.x, c.y - h, c.x + w, c.y + h * 0.55f, c.x - w, c.y + h * 0.55f);
    else
        p.addTriangle (c.x, c.y + h, c.x + w, c.y - h * 0.55f, c.x - w, c.y - h * 0.55f);

    g.setColour (down ? kAmber : kText);
    g.fillPath (p);
}

void StepLed::setOn (bool shouldBeOn)
{
    if (on == shouldBeOn)
        return;
    on = shouldBeOn;
    repaint();
}

void StepLed::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float side = juce::jmin (r.getWidth(), r.getHeight()) - 2.0f;
    r = r.withSizeKeepingCentre (side, side);
    g.setColour (on ? kLed : kWell);
    g.fillEllipse (r);
    g.setColour (on ? kLed.brighter (0.25f) : kLine);
    g.drawEllipse (r, 1.1f);
}

void StepButton::setCell (int v, bool drum909)
{
    value = v;
    is909 = drum909;
    repaint();
}

void StepButton::setDownbeat (bool isDownbeat)
{
    if (downbeat == isDownbeat)
        return;
    downbeat = isDownbeat;
    repaint();
}

void StepButton::paintButton (juce::Graphics& g, bool, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    juce::Colour c = downbeat ? kBeatWell : kWell;
    if (is909)
    {
        if (value == 1)
            c = kAmber;
        else if (value == 2)
            c = kVelHigh;
        else if (value >= 3)
            c = kGreen;
    }
    else if (value != 0)
    {
        c = kAmber;
    }
    g.setColour (c);
    g.fillRoundedRectangle (r, 3.0f);
}

ModloaderLookAndFeel::ModloaderLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, kText);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, kText);
    setColour (juce::ToggleButton::tickColourId, kAmber);
    setColour (juce::ToggleButton::tickDisabledColourId, kLine);
    setColour (juce::CaretComponent::caretColourId, kAmber);
    setColour (juce::TextButton::buttonColourId, kWell);
    setColour (juce::TextButton::textColourOffId, kText);
    setColour (juce::TextButton::textColourOnId, kAmber);
}

void ModloaderLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                             juce::Slider& slider)
{
    const float pad = width <= 22 ? 0.5f : (width < 34 ? 2.0f : 4.0f);
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (pad);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const bool red = (bool) slider.getProperties()["red"];

    g.setColour (kWell);
    g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
    g.setColour (kLine);
    g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.2f);

    const float arcInset = width <= 22 ? 1.2f : 2.5f;
    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius - arcInset, radius - arcInset, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (red ? kRed : kAmber);
    g.strokePath (arc, juce::PathStrokeType (width <= 22 ? 1.5f : 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path tick;
    tick.addRectangle (-1.0f, -radius + (width <= 22 ? 3.0f : 6.0f), 2.0f, radius * 0.4f);
    g.setColour (kText);
    g.fillPath (tick, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
}

void ModloaderLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                             bool highlighted, bool down)
{
    const bool tickRight = (bool) button.getProperties()["tickRight"];
    const bool radio = (bool) button.getProperties()["radio"];
    const bool caption = (bool) button.getProperties()["caption"];
    const auto fontSize = juce::jmin (15.0f, (float) button.getHeight() * 0.75f);
    const auto tickWidth = radio ? juce::jmin (11.0f, (float) button.getHeight() - 2.0f)
                                 : fontSize * 1.1f;
    const float tickX = tickRight ? (float) button.getWidth() - tickWidth - 4.0f : 4.0f;
    const float tickY = ((float) button.getHeight() - tickWidth) * 0.5f;

    if (radio)
    {
        auto r = juce::Rectangle<float> (tickX, tickY, tickWidth, tickWidth).reduced (1.0f);
        g.setColour (button.isEnabled() ? kLine : kLine.withAlpha (0.5f));
        g.drawEllipse (r, 1.1f);
        if (button.getToggleState())
        {
            g.setColour (button.isEnabled() ? kAmber : kAmber.withAlpha (0.5f));
            g.fillEllipse (r.reduced (2.6f));
        }
    }
    else
    {
        drawTickBox (g, button, tickX, tickY, tickWidth, tickWidth, button.getToggleState(),
                     button.isEnabled(), highlighted, down);
    }

    g.setColour (button.findColour (juce::ToggleButton::textColourId));
    g.setFont (caption ? juce::Font { juce::FontOptions().withHeight (11.0f).withStyle ("Bold") }
                       : juce::Font { juce::FontOptions (fontSize) });
    if (! button.isEnabled())
        g.setOpacity (0.5f);

    auto textBounds = button.getLocalBounds();
    if (tickRight)
        textBounds = textBounds.withTrimmedRight (juce::roundToInt (tickWidth) + 10).withTrimmedLeft (2);
    else
        textBounds = textBounds.withTrimmedLeft (juce::roundToInt (tickWidth) + 10).withTrimmedRight (2);

    g.drawFittedText (button.getButtonText(), textBounds,
                      tickRight ? juce::Justification::centredRight : juce::Justification::centredLeft, 10);
}

void ModloaderLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component&,
                                        float x, float y, float w, float h,
                                        bool ticked, bool isEnabled, bool, bool)
{
    auto bounds = juce::Rectangle<float> (x, y, w, h).reduced (1.0f);
    g.setColour (isEnabled ? kLine : kLine.withAlpha (0.5f));
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
    if (ticked)
    {
        g.setColour (isEnabled ? kAmber : kAmber.withAlpha (0.5f));
        auto tick = getTickShape (0.75f);
        g.fillPath (tick, tick.getTransformToScaleToFit (bounds.reduced (3.0f, 4.0f), false));
    }
}

juce::Button* ModloaderLookAndFeel::createSliderButton (juce::Slider&, bool isIncrement)
{
    return new IncDecArrowButton (isIncrement);
}

juce::Font ModloaderLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    if ((bool) button.getProperties()["headerBtn"])
        return withDefaultMetrics (juce::FontOptions (12.0f, juce::Font::bold));
    return LookAndFeel_V4::getTextButtonFont (button, buttonHeight);
}

juce::Font ModloaderLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

ModloaderEditor::ModloaderEditor (ModloaderProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      shuffleA (p.apvts, "shuffle", shuffle),
      pat808 (p.apvts, ModloaderProcessor::p808Id (0)),
      pat909 (p.apvts, ModloaderProcessor::p909Id (0))
{
    setLookAndFeel (&lnf);
    setSize (kEditorW, kEditorH);
    setResizable (false, false);

    auto addLabel = [this] (juce::Label& l, const juce::String& t, juce::Colour c, float h)
    {
        l.setText (t, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, c);
        l.setFont (juce::FontOptions().withHeight (h).withStyle ("Bold"));
        addAndMakeVisible (l);
    };

    addLabel (title, "OpenRB Modloader", kAmber, 18.0f);
    addLabel (modName, "Standard ReBirth", kText, 14.0f);
    addLabel (status, {}, kMuted, 12.0f);
    addLabel (shuffleN, "SHUFFLE", kMuted, 11.0f);
    addLabel (flamN, "FLAM", kMuted, 11.0f);
    flamN.setJustificationType (juce::Justification::centredLeft);
    modName.setJustificationType (juce::Justification::centredLeft);
    modName.setColour (juce::Label::backgroundColourId, kWell);
    modName.setColour (juce::Label::outlineColourId, kLine);
    status.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (modUp);
    addAndMakeVisible (modDown);
    for (auto* b : { &folderBtn, &datBtn })
    {
        b->setColour (juce::TextButton::buttonColourId, kWell);
        b->getProperties().set ("headerBtn", true);
        addAndMakeVisible (b);
    }
    modDown.onClick = [this] { loadMod (-1); };
    modUp.onClick = [this] { loadMod (1); };
    folderBtn.onClick = [this] { chooseModsFolder(); };
    datBtn.onClick = [this] { chooseDat(); };

    setupKnob (shuffle, false);

    setupPatternSection (0);
    setupPatternSection (1);
    setup808();
    setup909();
    setupDisable (disable808, 0);
    setupDisable (disable909, 1);
    select808 (1);
    select909 (1);
    addMouseListener (&focusListener, true);
    pat808.shownBank = currentPattern (0) / 8;
    pat909.shownBank = currentPattern (1) / 8;
    refreshPatternHeader();
    refreshModLabel();
    refreshSteps();
    resized();
    startTimerHz (30);
}

ModloaderEditor::~ModloaderEditor()
{
    stopTimer();
    removeMouseListener (&focusListener);
    setLookAndFeel (nullptr);
}

void ModloaderEditor::setupKnob (juce::Slider& s, bool red)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    s.setMouseDragSensitivity (110);
    if (red)
        s.getProperties().set ("red", true);
    addAndMakeVisible (s);
}

void ModloaderEditor::setupCaptionLabel (juce::Label& l, const juce::String& t)
{
    l.setText (t, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, kMuted);
    l.setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
    l.setJustificationType (juce::Justification::centred);
    l.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (l);
}

void ModloaderEditor::setupPatternSection (int machine)
{
    auto& ui = machine == 0 ? pat808 : pat909;
    ui.enable.getProperties().set ("caption", true);
    ui.enable.setColour (juce::ToggleButton::textColourId, kMuted);
    addAndMakeVisible (ui.enable);
    for (int i = 0; i < 4; ++i)
    {
        ui.bank[i].setButtonText (kBanks[i]);
        ui.bank[i].setClickingTogglesState (true);
        ui.bank[i].setRadioGroupId (20 + machine);
        addAndMakeVisible (ui.bank[i]);
        ui.bank[i].onClick = [this, machine, i]
        {
            auto& section = machine == 0 ? pat808 : pat909;
            section.shownBank = i;
        };
    }
    ui.bank[0].setToggleState (true, juce::dontSendNotification);
    for (int i = 0; i < 8; ++i)
    {
        ui.pat[i].setButtonText (juce::String (i + 1));
        ui.pat[i].setClickingTogglesState (true);
        ui.pat[i].setRadioGroupId (30 + machine);
        addAndMakeVisible (ui.pat[i]);
        ui.pat[i].onClick = [this, machine, i]
        {
            auto& section = machine == 0 ? pat808 : pat909;
            const int index = section.shownBank * 8 + i;
            if (auto* param = proc.apvts.getParameter (machine == 0 ? ModloaderProcessor::p808Id (1)
                                                                     : ModloaderProcessor::p909Id (1)))
                param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
            refreshPatternHeader();
            refreshSteps();
        };
    }
    ui.shuffleBtn.setClickingTogglesState (true);
    ui.shuffleBtn.getProperties().set ("caption", true);
    ui.shuffleBtn.setColour (juce::ToggleButton::textColourId, kMuted);
    addAndMakeVisible (ui.shuffleBtn);
    ui.shuffleBtn.onClick = [this, machine]
    {
        auto& section = machine == 0 ? pat808 : pat909;
        if (auto* e = eng())
            orb_modloader_set_pat_shuffle (e, machine, currentPattern (machine),
                                           section.shuffleBtn.getToggleState() ? 1 : 0);
    };

    ui.stepsLabel.setColour (juce::Label::textColourId, kMuted);
    ui.stepsLabel.setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
    ui.stepsLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (ui.stepsLabel);

    ui.stepsValue.setColour (juce::Label::textColourId, kText);
    ui.stepsValue.setColour (juce::Label::backgroundColourId, kWell);
    ui.stepsValue.setColour (juce::Label::outlineColourId, kLine);
    ui.stepsValue.setJustificationType (juce::Justification::centred);
    ui.stepsValue.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (ui.stepsValue);

    addAndMakeVisible (ui.stepsUp);
    addAndMakeVisible (ui.stepsDown);
    ui.stepsUp.onClick = [this, machine] { bumpLength (machine, 1); };
    ui.stepsDown.onClick = [this, machine] { bumpLength (machine, -1); };
}

void ModloaderEditor::setup808()
{
    for (int i = 0; i < ORB_MODLOADER_P808; ++i)
    {
        if (i < 2 || is808Switch (i))
            continue;
        setupKnob (k808[(size_t) i], is808Level (i));
        k808A[(size_t) i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, ModloaderProcessor::p808Id (i), k808[(size_t) i]);
        if (k808KnobLabel[i] != nullptr)
            setupCaptionLabel (n808[(size_t) i], k808KnobLabel[i]);
    }
    const char* swAlt[] = { "LC", "MC", "HC", "CL", "MA" };
    const char* swDef[] = { "LT", "MT", "HT", "RS", "CP" };
    for (int i = 0; i < 5; ++i)
    {
        auto setupRadio = [this, i] (juce::ToggleButton& b, const char* name)
        {
            b.setButtonText (name);
            b.setRadioGroupId (40 + i);
            b.setClickingTogglesState (true);
            b.getProperties().set ("radio", true);
            b.getProperties().set ("caption", true);
            b.setColour (juce::ToggleButton::textColourId, kMuted);
            addAndMakeVisible (b);
        };
        setupRadio (sw808[(size_t) i], swAlt[i]);
        setupRadio (sw808Def[(size_t) i], swDef[i]);
        // Engine selector set plays LT/MT/HT/RS/CP; clear plays LC/MC/HC/CL/MA.
        sw808A[(size_t) i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            proc.apvts, ModloaderProcessor::p808Id (kSwitch808[i]), sw808Def[(size_t) i]);
    }
    sync808Radios();
    for (int i = 0; i < 12; ++i)
    {
        legend808[(size_t) i].setText (k808Legend[i], juce::dontSendNotification);
        legend808[(size_t) i].setJustificationType (juce::Justification::centred);
        legend808[(size_t) i].setColour (juce::Label::textColourId, kMuted);
        legend808[(size_t) i].setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
        addAndMakeVisible (legend808[(size_t) i]);
        legend808[(size_t) i].addMouseListener (this, false);
    }
    for (int i = 0; i < 16; ++i)
    {
        addAndMakeVisible (step808[(size_t) i]);
        step808[(size_t) i].setDownbeat (i % 4 == 0);
        step808[(size_t) i].onClick = [this, i] { click808Step (i); };
        addAndMakeVisible (led808[(size_t) i]);
        led808[(size_t) i].setInterceptsMouseClicks (false, false);
    }
}

void ModloaderEditor::setup909()
{
    for (int i = 2; i < ORB_MODLOADER_P909; ++i)
    {
        setupKnob (k909[(size_t) i], i == 3 || i == 7 || i == 11 || i == 14 || i == 17 || i == 20
                                         || i == 21 || i == 22 || i == 25 || i == 27);
        k909A[(size_t) i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, ModloaderProcessor::p909Id (i), k909[(size_t) i]);
        if (k909KnobLabel[i] != nullptr)
            setupCaptionLabel (n909[(size_t) i], k909KnobLabel[i]);
    }
    for (int i = 0; i < 12; ++i)
    {
        legend909[(size_t) i].setText (k909Legend[i], juce::dontSendNotification);
        legend909[(size_t) i].setJustificationType (juce::Justification::centred);
        legend909[(size_t) i].setColour (juce::Label::textColourId, kMuted);
        legend909[(size_t) i].setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
        addAndMakeVisible (legend909[(size_t) i]);
        legend909[(size_t) i].addMouseListener (this, false);
    }
    for (int i = 0; i < 16; ++i)
    {
        addAndMakeVisible (step909[(size_t) i]);
        step909[(size_t) i].setDownbeat (i % 4 == 0);
        step909[(size_t) i].onClick = [this, i] { click909Step (i); };
        addAndMakeVisible (led909[(size_t) i]);
        led909[(size_t) i].setInterceptsMouseClicks (false, false);
    }
    flamEntry.setClickingTogglesState (true);
    flamEntry.setButtonText ({});
    flamEntry.setColour (juce::ToggleButton::textColourId, kText);
    addAndMakeVisible (flamEntry);
}

void ModloaderEditor::setupDisable (juce::ToggleButton& btn, int machine)
{
    btn.setClickingTogglesState (true);
    btn.setColour (juce::ToggleButton::textColourId, kMuted);
    btn.getProperties().set ("tickRight", true);
    btn.getProperties().set ("caption", true);
    addAndMakeVisible (btn);
    btn.onClick = [this, machine]
    {
        auto& button = machine == 0 ? disable808 : disable909;
        const int col = machine == 0 ? sel808 : sel909;
        const int bus = machine == 0 ? mixBusFor808 (col) : mixBusFor909 (col);
        if (bus < 0)
            return;
        if (auto* param = proc.apvts.getParameter (ModloaderProcessor::directId (bus)))
            param->setValueNotifyingHost (button.getToggleState() ? 1.0f : 0.0f);
        repaint();
    };
}

void ModloaderEditor::select808 (int col)
{
    sel808 = juce::jlimit (0, 11, col);
    for (int i = 0; i < 12; ++i)
        legend808[(size_t) i].setColour (juce::Label::textColourId, i == sel808 ? kAmber : kMuted);
    refreshSteps();
    refreshDisable();
    repaint();
}

void ModloaderEditor::select909 (int col)
{
    sel909 = juce::jlimit (0, 11, col);
    for (int i = 0; i < 12; ++i)
        legend909[(size_t) i].setColour (juce::Label::textColourId, i == sel909 ? kAmber : kMuted);
    refreshSteps();
    refreshDisable();
    repaint();
}

void ModloaderEditor::bumpLength (int machine, int delta)
{
    auto* e = eng();
    if (e == nullptr)
        return;
    const auto pat = currentPattern (machine);
    const int cur = orb_modloader_get_pat_length (e, machine, pat);
    orb_modloader_set_pat_length (e, machine, pat, (uint8_t) juce::jlimit (1, 16, cur + delta));
    refreshPatternHeader();
}

uint8_t ModloaderEditor::currentPattern (int machine) const
{
    const auto id = machine == 0 ? ModloaderProcessor::p808Id (1) : ModloaderProcessor::p909Id (1);
    if (auto* v = proc.apvts.getRawParameterValue (id))
        return (uint8_t) juce::jlimit (0, 31, juce::roundToInt (v->load()));
    return 0;
}

void ModloaderEditor::click808Step (int step)
{
    auto* e = eng();
    if (e == nullptr)
        return;
    const auto pat = currentPattern (0);
    const uint8_t cur = orb_modloader_get_step (e, 0, pat, (uint8_t) step, (uint8_t) sel808);
    orb_modloader_set_step (e, 0, pat, (uint8_t) step, (uint8_t) sel808, cur ? 0 : 1);
    refreshSteps();
}

void ModloaderEditor::click909Step (int step)
{
    auto* e = eng();
    if (e == nullptr)
        return;
    const auto pat = currentPattern (1);
    const uint8_t cur = orb_modloader_get_step (e, 1, pat, (uint8_t) step, (uint8_t) sel909);
    uint8_t next = 0;
    if (sel909 == 0)
        next = cur != 0 ? 0 : 2;
    else if (flamEntry.getToggleState())
        next = cur == 3 ? 0 : 3;
    else
        next = (uint8_t) ((cur + 1) % 3);
    orb_modloader_set_step (e, 1, pat, (uint8_t) step, (uint8_t) sel909, next);
    refreshSteps();
}

void ModloaderEditor::refreshSteps()
{
    auto* e = eng();
    if (e == nullptr)
        return;
    const auto p808 = currentPattern (0);
    const auto p909 = currentPattern (1);
    const int play808 = orb_modloader_last_step (e, 0);
    const int play909 = orb_modloader_last_step (e, 1);
    for (int i = 0; i < 16; ++i)
    {
        step808[(size_t) i].setCell (orb_modloader_get_step (e, 0, p808, (uint8_t) i, (uint8_t) sel808), false);
        step909[(size_t) i].setCell (orb_modloader_get_step (e, 1, p909, (uint8_t) i, (uint8_t) sel909), true);
        led808[(size_t) i].setOn (i == play808);
        led909[(size_t) i].setOn (i == play909);
    }
}

void ModloaderEditor::refreshPatternHeader()
{
    auto* e = eng();
    if (e == nullptr)
        return;
    for (int machine = 0; machine < 2; ++machine)
    {
        auto& ui = machine == 0 ? pat808 : pat909;
        const auto pat = currentPattern (machine);
        for (int i = 0; i < 4; ++i)
            ui.bank[i].setToggleState (i == ui.shownBank, juce::dontSendNotification);
        for (int i = 0; i < 8; ++i)
            ui.pat[i].setToggleState (ui.shownBank == pat / 8 && i == (pat % 8), juce::dontSendNotification);
        ui.shuffleBtn.setToggleState (orb_modloader_get_pat_shuffle (e, machine, pat) != 0,
                                      juce::dontSendNotification);
        ui.stepsValue.setText (juce::String ((int) orb_modloader_get_pat_length (e, machine, pat)),
                               juce::dontSendNotification);
    }
}

void ModloaderEditor::sync808Radios()
{
    for (int i = 0; i < 5; ++i)
        sw808[(size_t) i].setToggleState (! sw808Def[(size_t) i].getToggleState(),
                                         juce::dontSendNotification);
}

void ModloaderEditor::refreshDisable()
{
    auto sync = [this] (juce::ToggleButton& btn, int col, bool is808)
    {
        const int bus = is808 ? mixBusFor808 (col) : mixBusFor909 (col);
        const bool can = bus >= 0;
        btn.setEnabled (can);
        if (! can)
        {
            btn.setToggleState (false, juce::dontSendNotification);
            return;
        }
        bool on = false;
        if (auto* v = proc.apvts.getRawParameterValue (ModloaderProcessor::directId (bus)))
            on = v->load() >= 0.5f;
        btn.setToggleState (on, juce::dontSendNotification);
    };
    sync (disable808, sel808, true);
    sync (disable909, sel909, false);
}

void ModloaderEditor::refreshModLabel()
{
    auto* e = eng();
    if (e == nullptr)
        return;
    char buf[256] {};
    orb_modloader_mod_name (e, orb_modloader_current_mod (e), buf, (int) sizeof buf);
    modName.setText (juce::String::fromUTF8 (buf), juce::dontSendNotification);
    orb_modloader_status (e, buf, (int) sizeof buf);
    status.setText (juce::String::fromUTF8 (buf), juce::dontSendNotification);
}

void ModloaderEditor::loadMod (int delta)
{
    auto* e = eng();
    if (e == nullptr)
        return;
    const int n = orb_modloader_mod_count (e);
    if (n <= 0)
        return;
    int i = orb_modloader_current_mod (e) + delta;
    if (i < 0)
        i = n - 1;
    if (i >= n)
        i = 0;
    orb_modloader_load (e, i);
    refreshModLabel();
}

void ModloaderEditor::applyPickedMod (const juce::File& f)
{
    auto* e = eng();
    if (e == nullptr || !f.exists())
        return;

    const juce::File folder = f.isDirectory() ? f : f.getParentDirectory();
    proc.setModsFolder (folder.getFullPathName());

    if (f.existsAsFile())
    {
        const int n = orb_modloader_mod_count (e);
        char path[1024] {};
        for (int i = 0; i < n; ++i)
        {
            orb_modloader_mod_path (e, i, path, (int) sizeof path);
            if (f == juce::File (juce::String::fromUTF8 (path)))
            {
                orb_modloader_load (e, i);
                break;
            }
        }
    }

    refreshModLabel();
}

void ModloaderEditor::chooseModsFolder()
{
    auto* e = eng();
    char folder[1024] {};
    char current[1024] {};
    if (e)
    {
        orb_modloader_mods_folder (e, folder, (int) sizeof folder);
        orb_modloader_mod_path (e, orb_modloader_current_mod (e), current, (int) sizeof current);
    }
    juce::File start (current);
    if (!start.existsAsFile())
        start = juce::File (folder);

    chooser = std::make_unique<juce::FileChooser> (
        "ReBirth mod (.rbm) or Mods folder", start, "*.rbm;*.RBM");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              applyPickedMod (fc.getResult());
                          });
}

void ModloaderEditor::chooseDat()
{
    auto* e = eng();
    char folder[1024] {};
    char existing[1024] {};
    if (e)
    {
        orb_modloader_mods_folder (e, folder, (int) sizeof folder);
        orb_modloader_dat_path (e, existing, (int) sizeof existing);
    }
    juce::File start (existing);
    if (!start.existsAsFile())
    {
        const juce::File mods (folder);
        const auto beside = mods.getChildFile ("RB20FUL.DAT");
        const auto parent = mods.getParentDirectory().getChildFile ("RB20FUL.DAT");
        if (beside.existsAsFile())
            start = beside;
        else if (parent.existsAsFile())
            start = parent;
        else
            start = mods.getParentDirectory();
    }

    chooser = std::make_unique<juce::FileChooser> (
        "RB20FUL.DAT or ReBirth 2.x CD image", start, "*.DAT;*.dat;*.iso;*.ISO;*.cdr;*.CDR");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              auto* engine = eng();
                              if (!f.existsAsFile() || engine == nullptr)
                                  return;
                              proc.setKeyPath (f.getFullPathName());
                              orb_modloader_load (engine, orb_modloader_current_mod (engine));
                              refreshModLabel();
                          });
}

void ModloaderEditor::timerCallback()
{
    refreshSteps();
    refreshPatternHeader();
    refreshDisable();
    sync808Radios();
    refreshModLabel();
    repaint();
}

void ModloaderEditor::updateMachineFocus (juce::Point<int> pos)
{
    const int next = kMachine808.contains (pos) ? 0
                   : kMachine909.contains (pos) ? 1
                   : focusedMachine;
    if (next == focusedMachine)
        return;
    focusedMachine = next;
    repaint();
}

void ModloaderEditor::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < 12; ++i)
    {
        if (e.eventComponent == &legend808[(size_t) i])
        {
            select808 (i);
            return;
        }
        if (e.eventComponent == &legend909[(size_t) i])
        {
            select909 (i);
            return;
        }
    }
    const auto pos = e.getEventRelativeTo (this).getPosition();
    for (int i = 0; i < 12; ++i)
    {
        if (slot808[(size_t) i].contains (pos))
        {
            select808 (i);
            return;
        }
        if (slot909[(size_t) i].contains (pos))
        {
            select909 (i);
            return;
        }
    }
    AudioProcessorEditor::mouseDown (e);
}

void ModloaderEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    auto drawPanel = [&] (juce::Rectangle<int> r)
    {
        g.setColour (kPanel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (kLine);
        g.drawRoundedRectangle (r.toFloat(), 8.0f, 1.0f);
    };
    drawPanel ({ 8, 8, kEditorW - 16, 48 });
    drawPanel (kMachine808);
    drawPanel (kMachine909);

    if (focusedMachine >= 0)
    {
        const auto r = (focusedMachine == 0 ? kMachine808 : kMachine909).toFloat();
        g.setColour (kAmber);
        g.fillRect (r.getX() + 3.0f, r.getY() + 10.0f, 2.0f, r.getHeight() - 20.0f);
    }

    auto paintSlots = [&] (const std::array<juce::Rectangle<int>, 12>& slots)
    {
        for (int i = 0; i < 12; ++i)
        {
            auto r = slots[(size_t) i].toFloat();
            if (r.isEmpty())
                continue;
            g.setColour (i % 2 == 0 ? kSlotA : kSlotB);
            g.fillRect (r);
            g.setColour (kLine);
            g.fillRect (r.getX(), r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);
            if (i == 11)
                g.fillRect (r.getRight() - 1.0f, r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);
        }
    };
    paintSlots (slot808);
    paintSlots (slot909);

    auto markDisabled = [&] (const juce::Label& legend, int bus)
    {
        if (bus < 0)
            return;
        bool on = false;
        if (auto* v = proc.apvts.getRawParameterValue (ModloaderProcessor::directId (bus)))
            on = v->load() >= 0.5f;
        if (! on)
            return;
        auto r = legend.getBounds().toFloat();
        g.setColour (kGreen);
        g.fillRect (r.getX() + 10.0f, r.getBottom() + 1.0f, r.getWidth() - 20.0f, 1.6f);
    };
    for (int i = 0; i < 12; ++i)
    {
        markDisabled (legend808[(size_t) i], mixBusFor808 (i));
        markDisabled (legend909[(size_t) i], mixBusFor909 (i));
    }
}

void ModloaderEditor::resized()
{
    auto top = juce::Rectangle<int> (16, 12, kEditorW - 32, 40);
    title.setBounds (top.removeFromLeft (200));
    const int bandY = top.getY() + 6;
    const int bandH = top.getHeight() - 12;
    modName.setBounds (top.removeFromLeft (165).withY (bandY).withHeight (bandH));
    auto arrows = top.removeFromLeft (16).withY (bandY).withHeight (bandH);
    modUp.setBounds (arrows.removeFromTop (arrows.getHeight() / 2).reduced (1, 0));
    modDown.setBounds (arrows.reduced (1, 0));
    top.removeFromLeft (8);
    auto placeHeaderBtn = [this, &top, bandY, bandH] (juce::TextButton& b)
    {
        const auto font = lnf.getTextButtonFont (b, bandH);
        const int w = juce::GlyphArrangement::getStringWidthInt (font, b.getButtonText()) + 18;
        b.setBounds (top.removeFromLeft (w).withY (bandY).withHeight (bandH).reduced (1, 0));
    };
    placeHeaderBtn (folderBtn);
    top.removeFromLeft (2);
    placeHeaderBtn (datBtn);
    status.setBounds (top.removeFromLeft (180));
    shuffle.setBounds (top.removeFromRight (40).reduced (0, 2));
    shuffleN.setBounds (top.removeFromRight (64));

    auto placePattern = [this] (PatternUi& ui, juce::Rectangle<int> r, bool withFlam)
    {
        constexpr int gap = 2;
        auto grid = r.removeFromTop (56);
        const int pw = (grid.getWidth() - gap * 3) / 4;
        const int ph = (grid.getHeight() - gap) / 2;
        for (int i = 0; i < 8; ++i)
        {
            const int col = i % 4;
            const int row = i / 4;
            ui.pat[i].setBounds (grid.getX() + col * (pw + gap),
                                 grid.getY() + row * (ph + gap),
                                 pw, ph);
        }

        r.removeFromTop (8);
        auto banks = r.removeFromTop (24);
        const int bw = (banks.getWidth() - gap * 3) / 4;
        for (int i = 0; i < 4; ++i)
            ui.bank[i].setBounds (banks.getX() + i * (bw + gap), banks.getY(), bw, banks.getHeight());

        auto bottom = r.removeFromBottom (withFlam ? 114 : 78);
        if (withFlam)
        {
            auto flamRow = bottom.removeFromTop (36).reduced (1, 0);
            flamEntry.setBounds (flamRow.removeFromLeft (22).reduced (0, 6));
            flamN.setJustificationType (juce::Justification::centredLeft);
            auto flamLabel = flamRow.removeFromLeft (38);
            flamN.setBounds (flamLabel.withSizeKeepingCentre (flamLabel.getWidth(), 18));
            k909[29].setBounds (flamRow.removeFromLeft (kFlamKnob)
                                    .withSizeKeepingCentre (kFlamKnob, kFlamKnob));
        }
        auto shuffleRow = bottom.removeFromTop (26).reduced (1, 2);
        ui.shuffleBtn.setBounds (shuffleRow.withWidth (toggleContentWidth (ui.shuffleBtn, shuffleRow.getHeight())));
        bottom.removeFromTop (6);
        ui.stepsLabel.setBounds (bottom.removeFromTop (14));
        auto row = bottom.removeFromTop (24);
        ui.stepsValue.setBounds (row.removeFromLeft (34).reduced (1, 2));
        ui.stepsUp.setBounds (row.removeFromLeft (22).reduced (1));
        ui.stepsDown.setBounds (row.removeFromLeft (22).reduced (1));
    };

    const int machineRight = kEditorW - 16;
    constexpr int enableH = 22;
    constexpr int enableGap = 8;
    constexpr int leftColX = 18;
    constexpr int leftColW = 118;
    pat808.enable.setBounds (leftColX, 72, toggleContentWidth (pat808.enable, enableH), enableH);
    pat909.enable.setBounds (leftColX, 350, toggleContentWidth (pat909.enable, enableH), enableH);
    placePattern (pat808, { leftColX, 72 + enableH + enableGap, leftColW, 322 - (72 + enableH + enableGap) }, false);
    placePattern (pat909, { leftColX, 350 + enableH + enableGap, leftColW, 620 - (350 + enableH + enableGap) }, true);

    const int y808 = 76;
    const int y909 = 358;
    constexpr int ledH = 7;
    constexpr int stepH = 22;
    constexpr int legendH = 18;
    constexpr int ledGap = 6;
    constexpr int kSequencerBand = 60;
    const int contentBottom808 = 64 + 270 - 8;
    const int contentBottom909 = 342 + 290 - 8;

    auto placeLabeled = [] (juce::Slider& knob, juce::Label& label, int x, int y, int w)
    {
        label.setBounds (x, y, w, kLabelH);
        knob.setBounds (x + (w - kKnobSize) / 2, y + kLabelH, kKnobSize, kKnobSize);
    };

    auto place808Knob = [this, &placeLabeled] (int ctrl, int col, int row)
    {
        const int x = kSlot0X + col * k808SlotW;
        placeLabeled (k808[(size_t) ctrl], n808[(size_t) ctrl], x, y808 + row * kCtrlRow, k808SlotW);
    };
    auto place808Sw = [this] (int i, int col, int row)
    {
        const int slotX = kSlot0X + col * k808SlotW;
        const int y = y808 + row * kCtrlRow + 6;
        constexpr int h = 16;
        auto& alt = sw808[(size_t) i];
        auto& def = sw808Def[(size_t) i];
        const int w = juce::jmax (toggleContentWidth (alt, h), toggleContentWidth (def, h));
        const int x = slotX + (k808SlotW - w) / 2;
        alt.setBounds (x, y, w, h);
        def.setBounds (x, y + 16, w, h);
    };
    place808Knob (2, 0, 0);
    place808Knob (3, 1, 0);
    place808Knob (4, 1, 1);
    place808Knob (5, 1, 2);
    place808Knob (6, 2, 0);
    place808Knob (7, 2, 1);
    place808Knob (8, 2, 2);
    place808Knob (9, 3, 0);
    place808Knob (10, 3, 1);
    place808Sw (0, 3, 2);
    place808Knob (12, 4, 0);
    place808Knob (13, 4, 1);
    place808Sw (1, 4, 2);
    place808Knob (15, 5, 0);
    place808Knob (16, 5, 1);
    place808Sw (2, 5, 2);
    place808Knob (18, 6, 0);
    place808Sw (3, 6, 2);
    place808Knob (20, 7, 0);
    place808Sw (4, 7, 2);
    place808Knob (22, 8, 0);
    place808Knob (23, 9, 0);
    place808Knob (24, 9, 1);
    place808Knob (25, 9, 2);
    place808Knob (26, 10, 0);
    place808Knob (27, 10, 2);
    place808Knob (28, 11, 0);

    auto placeSlotsAndSequencer = [&] (std::array<juce::Label, 12>& legends,
                                       std::array<StepButton, 16>& steps,
                                       std::array<StepLed, 16>& leds,
                                       juce::ToggleButton& mute,
                                       std::array<juce::Rectangle<int>, 12>& slotRects,
                                       int y0, int contentBottom, bool is808)
    {
        const int slotBottom = contentBottom - kSequencerBand;
        const int legendY = slotBottom - legendH;
        const int clusterH = stepH + ledGap + ledH;
        const int clusterY = slotBottom + (kSequencerBand - clusterH) / 2 + 11;
        const int stepY = clusterY;
        const int ledY = clusterY + stepH + ledGap;
        for (int i = 0; i < 12; ++i)
        {
            const int x = is808 ? kSlot0X + i * k808SlotW : slot909X (i);
            const int w = is808 ? k808SlotW : k909SlotW[i];
            slotRects[(size_t) i] = { x, y0 - 6, w, slotBottom - (y0 - 6) };
            legends[(size_t) i].setBounds (x, legendY, w, legendH);
        }
        for (int i = 0; i < 16; ++i)
        {
            steps[(size_t) i].setBounds (kSlot0X + i * 46, stepY, 42, stepH);
            leds[(size_t) i].setBounds (kSlot0X + i * 46 + 17, ledY, 7, ledH);
        }
        const int muteW = toggleContentWidth (mute, stepH);
        mute.setBounds (machineRight - muteW, stepY, muteW, stepH);
    };
    placeSlotsAndSequencer (legend808, step808, led808, disable808, slot808,
                            y808, contentBottom808, true);

    auto place909Knob = [this, &placeLabeled] (int ctrl, int col, int row, SlotSide side)
    {
        const int slotX = slot909X (col);
        const int slotW = k909SlotW[col];
        const int y = y909 + row * kCtrlRow;
        int x = slotX, w = slotW;
        if (side != SlotSide::Centre)
        {
            constexpr int pad = 4;
            w = (slotW - pad * 2) / 2;
            x = side == SlotSide::Left ? slotX + pad : slotX + pad + w;
        }
        placeLabeled (k909[(size_t) ctrl], n909[(size_t) ctrl], x, y, w);
    };
    place909Knob (2, 0, 0, SlotSide::Centre);
    place909Knob (4, 1, 0, SlotSide::Left);
    place909Knob (3, 1, 0, SlotSide::Right);
    place909Knob (5, 1, 1, SlotSide::Left);
    place909Knob (6, 1, 1, SlotSide::Right);
    place909Knob (8, 2, 0, SlotSide::Left);
    place909Knob (7, 2, 0, SlotSide::Right);
    place909Knob (9, 2, 1, SlotSide::Left);
    place909Knob (10, 2, 1, SlotSide::Right);
    place909Knob (12, 3, 0, SlotSide::Left);
    place909Knob (11, 3, 0, SlotSide::Right);
    place909Knob (13, 3, 1, SlotSide::Left);
    place909Knob (15, 4, 0, SlotSide::Left);
    place909Knob (14, 4, 0, SlotSide::Right);
    place909Knob (16, 4, 1, SlotSide::Left);
    place909Knob (18, 5, 0, SlotSide::Left);
    place909Knob (17, 5, 0, SlotSide::Right);
    place909Knob (19, 5, 1, SlotSide::Left);
    place909Knob (20, 6, 0, SlotSide::Centre);
    place909Knob (21, 7, 0, SlotSide::Centre);
    place909Knob (23, 8, 1, SlotSide::Centre);
    place909Knob (22, 9, 0, SlotSide::Centre);
    place909Knob (24, 9, 1, SlotSide::Centre);
    place909Knob (25, 10, 0, SlotSide::Centre);
    place909Knob (26, 10, 1, SlotSide::Centre);
    place909Knob (27, 11, 0, SlotSide::Centre);
    place909Knob (28, 11, 1, SlotSide::Centre);

    placeSlotsAndSequencer (legend909, step909, led909, disable909, slot909,
                            y909, contentBottom909, false);
}
