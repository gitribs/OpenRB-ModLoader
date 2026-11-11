#include "PluginEditor.h"
#include "PatternMidi.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
constexpr int kSplashW = 98;
constexpr int kSplashH = 65;
constexpr int kSplashGap = 10;
const juce::Rectangle<int> kMachine808 { 8, 64, kEditorW - 16, 270 };
const juce::Rectangle<int> kMachine909 { 8, 342, kEditorW - 16, 290 };
constexpr int k909OuterPad = 16;
constexpr int k909PairHalf = 40;
constexpr int k909InnerPad = (k909PairHalf - kKnobSize) / 2;
constexpr int k909PairPad = k909OuterPad - k909InnerPad;
constexpr int k909SplitW = k909PairPad * 2 + k909PairHalf * 2;
constexpr int k909CentreW = k909OuterPad * 2 + kKnobSize;
constexpr int k909HatW = k909PairHalf + k909OuterPad - k909InnerPad;
constexpr int k909SlotW[] = {
    k909CentreW, k909SplitW, k909SplitW, k909SplitW, k909SplitW, k909SplitW,
    k909CentreW, k909CentreW, k909HatW, k909HatW, k909CentreW, k909CentreW
};
constexpr int k909SlotAreaW = k909CentreW * 5 + k909SplitW * 5 + k909HatW * 2;
constexpr int k808BaseW = k909SlotAreaW / 12;
constexpr int k808SlotW[] = {
    k808BaseW + 1, k808BaseW + 1, k808BaseW,
    k808BaseW + 1, k808BaseW + 1, k808BaseW,
    k808BaseW + 1, k808BaseW + 1, k808BaseW,
    k808BaseW + 1, k808BaseW + 1, k808BaseW
};
static_assert (k909SlotAreaW == 812);
static_assert (k808BaseW * 12 + 8 == k909SlotAreaW);
constexpr int kFaderColW = 20;
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

int slotX (const int* widths, int col)
{
    int x = kSlot0X;
    for (int i = 0; i < col; ++i)
        x += widths[i];
    return x;
}

int slot808X (int col) { return slotX (k808SlotW, col); }
int slot909X (int col) { return slotX (k909SlotW, col); }

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

bool songDistOn (const OrbModloaderSong& s, int machine)
{
    return s.dist_enabled != 0 && (machine == 0 ? s.dist_808 : s.dist_909) != 0;
}

bool songPcfOn (const OrbModloaderSong& s, int machine)
{
    return s.pcf_enabled != 0 && s.pcf_id == (machine == 0 ? 3 : 4);
}

bool songDelayOn (const OrbModloaderSong& s, int machine)
{
    return s.delay_enabled != 0 && (machine == 0 ? s.delay_send_808 : s.delay_send_909) != 0;
}

uint8_t delaySendOf (const OrbModloaderSong& s, int machine)
{
    return machine == 0 ? s.delay_send_808 : s.delay_send_909;
}

juce::String distortRecallText (const OrbModloaderSong& s)
{
    return "openrb-distort 1\namount: " + juce::String ((int) s.dist_amount)
         + "\nshape: " + juce::String ((int) s.dist_shape) + "\n";
}

juce::String pcfRecallText (const OrbModloaderSong& s)
{
    const int pattern = juce::jlimit (0, 54, (int) s.pcf_wave);
    return "openrb-pcf 1\nfrequency: " + juce::String ((int) s.pcf_frequency)
         + "\nresonance: " + juce::String ((int) s.pcf_resonance)
         + "\namount: " + juce::String ((int) s.pcf_amount)
         + "\ndecay: " + juce::String ((int) s.pcf_decay)
         + "\npattern: " + juce::String (pattern)
         + "\nmode: " + juce::String (s.pcf_mode == 1 ? "lp" : "bp") + "\n";
}

juce::String delayRecallText (const OrbModloaderSong& s, int machine)
{
    return "openrb-delay 1\nmix: " + juce::String ((int) delaySendOf (s, machine))
         + "\nsteps: " + juce::String ((int) s.delay_steps)
         + "\nmode: " + juce::String (s.delay_mode == 0 ? "triplet" : "sixteenth")
         + "\nfeedback: " + juce::String ((int) s.delay_feedback)
         + "\npan: " + juce::String ((int) s.delay_pan) + "\n";
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

constexpr int kPatLen = ModloaderProcessor::kPatternLen;
constexpr int kSteps = 16;
constexpr int kCols = 12;
constexpr int k909CH = 8;
constexpr int k909OH = 9;

uint8_t& patCell (uint8_t* p, int step, int col)
{
    return p[2 + kCols * step + col];
}

void clearSteps (uint8_t* p)
{
    std::memset (p + 2, 0, (size_t) (kPatLen - 2));
}

void shiftColumn (uint8_t* p, int col, bool right)
{
    if (right)
    {
        const auto last = patCell (p, kSteps - 1, col);
        for (int s = kSteps - 1; s > 0; --s)
            patCell (p, s, col) = patCell (p, s - 1, col);
        patCell (p, 0, col) = last;
        return;
    }
    const auto first = patCell (p, 0, col);
    for (int s = 0; s < kSteps - 1; ++s)
        patCell (p, s, col) = patCell (p, s + 1, col);
    patCell (p, kSteps - 1, col) = first;
}

uint8_t randomCell (bool is909, int col, juce::Random& rng)
{
    if (! is909)
        return (uint8_t) rng.nextInt (2);
    if (col == 0)
        return rng.nextInt (2) != 0 ? (uint8_t) 2 : 0;
    return (uint8_t) rng.nextInt (4);
}

void shuffleColumn (uint8_t* p, int col, juce::Random& rng)
{
    uint8_t v[kSteps];
    for (int s = 0; s < kSteps; ++s)
        v[s] = patCell (p, s, col);
    for (int i = kSteps - 1; i > 0; --i)
        std::swap (v[i], v[rng.nextInt (i + 1)]);
    for (int s = 0; s < kSteps; ++s)
        patCell (p, s, col) = v[s];
}

void exclusiveHats (uint8_t* p, int preferCol)
{
    for (int s = 0; s < kSteps; ++s)
    {
        auto& ch = patCell (p, s, k909CH);
        auto& oh = patCell (p, s, k909OH);
        if (ch == 0 || oh == 0)
            continue;
        if (preferCol == k909CH)
            oh = 0;
        else
            ch = 0;
    }
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

juce::Font captionFont()
{
    return juce::Font { juce::FontOptions().withHeight (11.0f).withStyle ("Bold") };
}

int captionLabelWidth (const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidthInt (captionFont(), text);
}
} // namespace

void ModSlider::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Slider::mouseDown (e);
}

void ModSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Slider::mouseDrag (e);
}

void ModSlider::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Slider::mouseUp (e);
}

bool ModSlider::hitTest (int x, int y)
{
    if (! (bool) getProperties()["fader"])
        return juce::Slider::hitTest (x, y);
    return x >= 0 && x < kFaderColW && y >= 0 && y < getHeight();
}

void ModTextButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::TextButton::mouseDown (e);
}

void ModTextButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::TextButton::mouseUp (e);
}

void ModToggleButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::ToggleButton::mouseDown (e);
}

void ModToggleButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::ToggleButton::mouseUp (e);
}

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

void IncDecArrowButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Button::mouseDown (e);
}

void IncDecArrowButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Button::mouseUp (e);
}

HeaderIconButton::HeaderIconButton (Kind k) : juce::Button ({}), kind (k) {}

void HeaderIconButton::setAttention (bool shouldAttention)
{
    if (attention == shouldAttention)
        return;
    attention = shouldAttention;
    repaint();
}

void HeaderIconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (kWell);
    g.fillRoundedRectangle (r, 2.0f);
    const bool lit = highlighted || down || attention;
    g.setColour (lit ? kAmber : kLine);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    g.setColour (down || attention ? kAmber : kText);
    const auto c = r.getCentre();

    if (kind == Kind::Settings)
    {
        const float radius = juce::jmin (r.getWidth(), r.getHeight()) * 0.28f;
        const float inner = radius * 0.62f;
        const float hole = radius * 0.32f;
        constexpr int teeth = 8;
        juce::Path gear;
        for (int i = 0; i < teeth; ++i)
        {
            const float step = juce::MathConstants<float>::twoPi / (float) teeth;
            const float a = (float) i * step - juce::MathConstants<float>::halfPi;
            const float a0 = a - step * 0.16f;
            const float a1 = a + step * 0.16f;
            const float b0 = a + step * 0.34f;
            const float b1 = a + step * 0.66f;
            auto pt = [c] (float ang, float rad)
            {
                return juce::Point<float> (c.x + std::cos (ang) * rad, c.y + std::sin (ang) * rad);
            };
            if (i == 0)
                gear.startNewSubPath (pt (a0, radius));
            gear.lineTo (pt (a0, radius));
            gear.lineTo (pt (a1, radius));
            gear.lineTo (pt (b0, inner));
            gear.lineTo (pt (b1, inner));
        }
        gear.closeSubPath();
        gear.setUsingNonZeroWinding (false);
        gear.addEllipse (c.x - hole, c.y - hole, hole * 2.0f, hole * 2.0f);
        g.fillPath (gear);
        return;
    }

    if (kind == Kind::Open)
    {
        const float w = juce::jmin (r.getWidth(), r.getHeight()) * 0.52f;
        const float h = w * 0.72f;
        auto body = juce::Rectangle<float> (c.x - w * 0.5f, c.y - h * 0.28f, w, h * 0.78f);
        auto tab = juce::Rectangle<float> (body.getX(), body.getY() - h * 0.28f, w * 0.42f, h * 0.36f);
        juce::Path folder;
        folder.addRoundedRectangle (tab, 1.4f);
        folder.addRoundedRectangle (body, 1.6f);
        g.fillPath (folder);
        return;
    }

    const float w = juce::jmin (r.getWidth(), r.getHeight()) * 0.50f;
    const float h = w * 1.05f;
    auto disk = juce::Rectangle<float> (c.x - w * 0.5f, c.y - h * 0.5f, w, h);
    juce::Path floppy;
    floppy.addRoundedRectangle (disk, 1.4f);
    g.fillPath (floppy);
    g.setColour (kWell);
    auto shutter = disk.removeFromTop (h * 0.34f).reduced (w * 0.16f, h * 0.06f);
    g.fillRect (shutter);
    auto label = disk.removeFromBottom (h * 0.28f).reduced (w * 0.14f, 0.0f);
    g.fillRect (label);
}

void HeaderIconButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Button::mouseDown (e);
}

FxBadgeButton::FxBadgeButton() : juce::Button ({}) {}

void FxBadgeButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (kWell);
    g.fillRoundedRectangle (r, 2.0f);
    const auto ink = highlighted || down ? kRed.brighter (0.25f) : kRed;
    g.setColour (ink);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
    g.setFont (juce::Font { juce::FontOptions().withHeight (11.0f).withStyle ("Bold") });
    g.drawText ("FX", getLocalBounds(), juce::Justification::centred, false);
}

void FxBadgeButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Button::mouseDown (e);
}

void FxBadgeButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Button::mouseUp (e);
}

void HeaderIconButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::Button::mouseUp (e);
}

void MidiDragHandle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (kWell);
    g.fillRoundedRectangle (r, 2.0f);
    const bool lit = isEnabled() && (isMouseOverOrDragging (true) || dragging);
    g.setColour (! isEnabled() ? kLine.withAlpha (0.4f) : (lit ? kAmber : kLine));
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    const auto c = r.getCentre();
    const float outer = juce::jmin (r.getWidth(), r.getHeight()) * 0.32f;
    const float inner = outer * 0.62f;
    g.setColour (! isEnabled() ? kMuted.withAlpha (0.45f) : (lit ? kAmber : kText));
    g.drawEllipse (c.x - outer, c.y - outer, outer * 2.0f, outer * 2.0f, 1.2f);
    g.drawEllipse (c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f, 1.0f);

    const float pin = juce::jmax (1.4f, outer * 0.16f);
    const float ring = inner * 0.72f;
    constexpr float kHalfPi = juce::MathConstants<float>::halfPi;
    constexpr float kSpread = 0.85f;
    const float angles[] = {
        kHalfPi - kSpread, kHalfPi, kHalfPi + kSpread,
        kHalfPi * 3.0f - kSpread, kHalfPi * 3.0f + kSpread
    };
    for (float a : angles)
    {
        const float x = c.x + std::cos (a) * ring;
        const float y = c.y + std::sin (a) * ring;
        g.fillEllipse (x - pin * 0.5f, y - pin * 0.5f, pin, pin);
    }
}

void MidiDragHandle::mouseDown (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
}

void MidiDragHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! isEnabled() || dragging || onStartDrag == nullptr)
        return;
    if (e.getDistanceFromDragStart() < 4)
        return;
    dragging = true;
    repaint();
    onStartDrag();
    dragging = false;
    repaint();
}

class ModloaderEditor::ModPicker final : public juce::Component,
                                        public juce::ListBoxModel
{
public:
    explicit ModPicker (ModloaderEditor& o) : owner (o)
    {
        setWantsKeyboardFocus (true);
        list.setModel (this);
        list.setRowHeight (kRowH);
        list.setOutlineThickness (0);
        list.setColour (juce::ListBox::backgroundColourId, kPanel);
        list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        list.setMouseMoveSelectsRows (true);
        list.setWantsKeyboardFocus (true);
        if (auto* vp = list.getViewport())
            vp->setScrollBarsShown (false, false, true, false);
        addAndMakeVisible (list);
    }

    ~ModPicker() override
    {
        list.setModel (nullptr);
    }

    void setMods (juce::StringArray names, int selected)
    {
        mods = std::move (names);
        current = -1;
        list.updateContent();
        syncSelection (selected);
    }

    void syncSelection (int index)
    {
        index = juce::jlimit (0, juce::jmax (0, mods.size() - 1), index);
        if (current == index)
            return;
        current = index;
        list.selectRow (current, false, false);
        list.scrollToEnsureRowIsOnscreen (current);
        list.repaint();
    }

    int preferredWidth (int minWidth) const
    {
        const auto font = juce::Font { juce::FontOptions (13.0f) };
        int w = minWidth;
        for (const auto& name : mods)
            w = juce::jmax (w, juce::GlyphArrangement::getStringWidthInt (font, name) + 24);
        return juce::jmin (w, 360);
    }

    int preferredHeight (int maxHeight) const
    {
        const int content = mods.size() * kRowH + 2;
        return juce::jlimit (kRowH + 2, maxHeight, content);
    }

    int getNumRows() override { return mods.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, mods.size()))
            return;
        if (selected)
        {
            g.setColour (kAmber);
            g.fillRect (0, 0, width, height);
        }
        else if (row == current)
        {
            g.setColour (kWell);
            g.fillRect (0, 0, width, height);
        }
        g.setColour (selected ? kBg : kText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (mods[row], 8, 0, width - 16, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        pick (row);
    }

    void returnKeyPressed (int lastRowSelected) override
    {
        pick (lastRowSelected);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (kPanel);
        g.fillRect (getLocalBounds());
        g.setColour (kLine);
        g.drawRect (getLocalBounds(), 1);
    }

    void resized() override
    {
        list.setBounds (getLocalBounds().reduced (1));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            owner.hideModPicker();
            return true;
        }
        return false;
    }

    void inputAttemptWhenModal() override
    {
        owner.hideModPicker();
    }

    void focusGained (FocusChangeType) override
    {
        list.grabKeyboardFocus();
    }

    bool canModalEventBeSentToComponent (const juce::Component* target) override
    {
        return target == &owner.modName || target == &owner.modUp || target == &owner.modDown
            || target == &owner.settingsBtn;
    }

private:
    void pick (int row)
    {
        if (! juce::isPositiveAndBelow (row, mods.size()))
            return;
        owner.loadModIndex (row);
        owner.hideModPicker();
    }

    static constexpr int kRowH = 22;
    ModloaderEditor& owner;
    juce::ListBox list;
    juce::StringArray mods;
    int current = 0;
};

class ModloaderEditor::SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (ModloaderEditor& o, bool locate) : owner (o), locateDat (locate)
    {
        setWantsKeyboardFocus (true);
        setOpaque (false);

        auto setupLabel = [this] (juce::Label& l, const juce::String& t, juce::Colour c, float h, bool bold)
        {
            l.setText (t, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, c);
            l.setFont (bold ? juce::Font { juce::FontOptions().withHeight (h).withStyle ("Bold") }
                            : juce::Font { juce::FontOptions (h) });
            l.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (l);
        };

        setupLabel (title, "Settings", kAmber, 16.0f, true);
        setupLabel (message, {}, kAmber, 13.0f, false);
        message.setJustificationType (juce::Justification::topLeft);
        message.setMinimumHorizontalScale (1.0f);
        message.setVisible (false);
        setupLabel (filesHeading, "File Locations", kMuted, 12.0f, true);
        setupLabel (modsCaption, "Mods Folder", kText, 12.0f, false);
        setupLabel (datCaption, "DAT / ISO", kText, 12.0f, false);

        auto setupPath = [this] (juce::Label& l)
        {
            l.setColour (juce::Label::backgroundColourId, kWell);
            l.setColour (juce::Label::outlineColourId, kLine);
            l.setColour (juce::Label::textColourId, kText);
            l.setFont (juce::FontOptions (12.0f));
            l.setJustificationType (juce::Justification::centredLeft);
            l.setBorderSize ({ 0, 8, 0, 8 });
            l.setMinimumHorizontalScale (0.7f);
            addAndMakeVisible (l);
        };
        setupPath (modsPath);
        setupPath (datPath);

        auto setupBtn = [this] (juce::TextButton& b, const juce::String& t)
        {
            b.setButtonText (t);
            b.setColour (juce::TextButton::buttonColourId, kWell);
            b.getProperties().set ("headerBtn", true);
            addAndMakeVisible (b);
        };
        setupBtn (closeBtn, juce::String::fromUTF8 ("\xc3\x97"));
        setupBtn (modsBtn, "Choose...");
        setupBtn (datBtn, "Choose...");
        closeBtn.onClick = [this] { owner.hideSettings(); };
        modsBtn.onClick = [this] { owner.chooseModsFolder(); };
        datBtn.onClick = [this] { owner.chooseDat(); };
        refresh();
    }

    void setLocateDat (bool shouldLocate)
    {
        locateDat = shouldLocate;
        refresh();
    }

    void refresh()
    {
        auto* e = owner.eng();
        char folder[1024] {};
        char dat[1024] {};
        if (e != nullptr)
        {
            orb_modloader_mods_folder (e, folder, (int) sizeof folder);
            orb_modloader_dat_path (e, dat, (int) sizeof dat);
        }
        setPathText (modsPath, juce::String::fromUTF8 (folder));
        setPathText (datPath, juce::String::fromUTF8 (dat));
        datPath.setColour (juce::Label::outlineColourId, locateDat ? kAmber : kLine);

        if (locateDat)
        {
            message.setText (owner.datLocateMessage(), juce::dontSendNotification);
            message.setVisible (true);
        }
        else
        {
            message.setText ({}, juce::dontSendNotification);
            message.setVisible (false);
        }
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (getLocalBounds());
        auto card = cardBounds().toFloat();
        g.setColour (kPanel);
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (kLine);
        g.drawRoundedRectangle (card, 8.0f, 1.0f);
    }

    void resized() override
    {
        auto r = cardBounds().reduced (20, 16);
        auto titleRow = r.removeFromTop (24);
        closeBtn.setBounds (titleRow.removeFromRight (24).reduced (1));
        title.setBounds (titleRow);
        r.removeFromTop (12);
        if (message.isVisible())
        {
            message.setBounds (r.removeFromTop (48));
            r.removeFromTop (12);
        }
        filesHeading.setBounds (r.removeFromTop (18));
        r.removeFromTop (8);
        layoutFileRow (r, modsCaption, modsPath, modsBtn);
        r.removeFromTop (10);
        layoutFileRow (r, datCaption, datPath, datBtn);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! cardBounds().contains (e.getPosition()) && ! owner.fileChooserOpen)
            owner.hideSettings();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            owner.hideSettings();
            return true;
        }
        return false;
    }

    void inputAttemptWhenModal() override
    {
        if (! owner.fileChooserOpen)
            owner.hideSettings();
    }

    bool canModalEventBeSentToComponent (const juce::Component* target) override
    {
        return target == &owner.settingsBtn
            || target == &owner.fx808
            || target == &owner.fx909;
    }

private:
    juce::Rectangle<int> cardBounds() const
    {
        const int h = message.isVisible() ? 268 : 216;
        return getLocalBounds().withSizeKeepingCentre (kCardW, h);
    }

    static void layoutFileRow (juce::Rectangle<int>& r, juce::Label& caption, juce::Label& path,
                               juce::TextButton& btn)
    {
        caption.setBounds (r.removeFromTop (16));
        r.removeFromTop (4);
        auto row = r.removeFromTop (28);
        btn.setBounds (row.removeFromRight (88).reduced (0, 1));
        row.removeFromRight (8);
        path.setBounds (row);
    }

    static void setPathText (juce::Label& l, const juce::String& path)
    {
        if (path.isEmpty())
        {
            l.setText ("Not set", juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, kMuted);
            l.setTooltip ({});
            return;
        }
        l.setText (path, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, kText);
        l.setTooltip (path);
    }

    static constexpr int kCardW = 460;
    ModloaderEditor& owner;
    juce::Label title, message, filesHeading;
    juce::Label modsCaption, modsPath, datCaption, datPath;
    ModTextButton closeBtn, modsBtn, datBtn;
    bool locateDat = false;
};

class ModloaderEditor::EffectsRecallPanel final : public juce::Component
{
public:
    EffectsRecallPanel (ModloaderEditor& o, int machineId) : owner (o), machine (machineId)
    {
        setWantsKeyboardFocus (true);
        setOpaque (false);

        auto setupLabel = [this] (juce::Label& l, juce::Colour c, float h, bool bold)
        {
            l.setColour (juce::Label::textColourId, c);
            l.setFont (bold ? juce::Font { juce::FontOptions().withHeight (h).withStyle ("Bold") }
                            : juce::Font { juce::FontOptions (h) });
            l.setJustificationType (juce::Justification::topLeft);
            l.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (l);
        };

        setupLabel (title, kAmber, 16.0f, true);
        setupLabel (routing, kText, 13.0f, false);
        routing.setJustificationType (juce::Justification::topLeft);

        auto setupRow = [this, &setupLabel] (Row& row, const juce::String& name)
        {
            setupLabel (row.title, kAmber, 13.0f, true);
            row.title.setText (name, juce::dontSendNotification);
            setupLabel (row.summary, kText, 12.0f, false);
            row.copy.setButtonText ("Copy");
            row.copy.setColour (juce::TextButton::buttonColourId, kWell);
            row.copy.getProperties().set ("headerBtn", true);
            addAndMakeVisible (row.copy);
        };
        setupRow (dist, "Distortion");
        setupRow (pcf, "PCF");
        setupRow (delay, "Delay");
        dist.copy.onClick = [this] { copyText (distortRecallText (owner.proc.songEffects())); };
        pcf.copy.onClick = [this] { copyText (pcfRecallText (owner.proc.songEffects())); };
        delay.copy.onClick = [this]
        {
            copyText (delayRecallText (owner.proc.songEffects(), machine));
        };

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, kWell);
        closeBtn.getProperties().set ("headerBtn", true);
        addAndMakeVisible (closeBtn);
        closeBtn.onClick = [this] { owner.hideEffectsRecall(); };
        refresh();
    }

    int currentMachine() const { return machine; }

    void setMachine (int machineId)
    {
        if (machine == machineId)
            return;
        machine = machineId;
        refresh();
    }

    void refresh()
    {
        const auto& s = owner.proc.songEffects();
        const bool is808 = machine == 0;
        title.setText (is808 ? "808 effects" : "909 effects", juce::dontSendNotification);
        routing.setText ("Insert OpenRB effects on the "
                             + juce::String (is808 ? "808" : "909")
                             + " Mix bus, Distortion then PCF, then Delay. Copy each used effect "
                               "and paste it into the matching OpenRB plugin.",
                         juce::dontSendNotification);

        dist.visible = songDistOn (s, machine);
        dist.summary.setText ("amount " + juce::String ((int) s.dist_amount)
                                  + "    shape " + juce::String ((int) s.dist_shape),
                              juce::dontSendNotification);

        pcf.visible = songPcfOn (s, machine);
        pcf.summary.setText ("freq " + juce::String ((int) s.pcf_frequency)
                                 + "    res " + juce::String ((int) s.pcf_resonance)
                                 + "    amt " + juce::String ((int) s.pcf_amount)
                                 + "    decay " + juce::String ((int) s.pcf_decay)
                                 + "    pattern " + juce::String (juce::jlimit (0, 54, (int) s.pcf_wave))
                                 + "    mode " + juce::String (s.pcf_mode == 1 ? "LP" : "BP"),
                             juce::dontSendNotification);

        delay.visible = songDelayOn (s, machine);
        delay.summary.setText ("mix " + juce::String ((int) delaySendOf (s, machine))
                                   + "    steps " + juce::String ((int) s.delay_steps)
                                   + "    mode " + juce::String (s.delay_mode == 0 ? "triplet" : "sixteenth")
                                   + "    feedback " + juce::String ((int) s.delay_feedback)
                                   + "    pan " + juce::String ((int) s.delay_pan),
                               juce::dontSendNotification);

        auto showRow = [] (Row& row)
        {
            row.title.setVisible (row.visible);
            row.summary.setVisible (row.visible);
            row.copy.setVisible (row.visible);
        };
        showRow (dist);
        showRow (pcf);
        showRow (delay);
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (getLocalBounds());
        auto card = cardBounds().toFloat();
        g.setColour (kPanel);
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (kLine);
        g.drawRoundedRectangle (card, 8.0f, 1.0f);
    }

    void resized() override
    {
        auto r = cardBounds().reduced (20, 16);
        auto titleRow = r.removeFromTop (24);
        closeBtn.setBounds (titleRow.removeFromRight (24).reduced (1));
        title.setBounds (titleRow);
        r.removeFromTop (12);
        routing.setBounds (r.removeFromTop (64));
        r.removeFromTop (12);
        layoutRow (r, dist);
        layoutRow (r, pcf);
        layoutRow (r, delay);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! cardBounds().contains (e.getPosition()))
            owner.hideEffectsRecall();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            owner.hideEffectsRecall();
            return true;
        }
        return false;
    }

    void inputAttemptWhenModal() override
    {
        owner.hideEffectsRecall();
    }

    bool canModalEventBeSentToComponent (const juce::Component* target) override
    {
        return target == &owner.fx808 || target == &owner.fx909 || target == &owner.settingsBtn;
    }

private:
    struct Row
    {
        juce::Label title, summary;
        ModTextButton copy;
        bool visible = false;
    };

    juce::Rectangle<int> cardBounds() const
    {
        const int n = (int) dist.visible + (int) pcf.visible + (int) delay.visible;
        const int h = 148 + juce::jmax (1, n) * 58;
        return getLocalBounds().withSizeKeepingCentre (kCardW, h);
    }

    static void layoutRow (juce::Rectangle<int>& r, Row& row)
    {
        if (! row.visible)
            return;
        auto head = r.removeFromTop (22);
        row.copy.setBounds (head.removeFromRight (72).reduced (0, 1));
        head.removeFromRight (8);
        row.title.setBounds (head);
        r.removeFromTop (4);
        row.summary.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);
    }

    static void copyText (const juce::String& text)
    {
        juce::SystemClipboard::copyTextToClipboard (text);
    }

    static constexpr int kCardW = 520;
    ModloaderEditor& owner;
    int machine = 0;
    juce::Label title, routing;
    Row dist, pcf, delay;
    ModTextButton closeBtn;
};

class ModloaderEditor::SplashViewer final : public juce::Component
{
public:
    explicit SplashViewer (ModloaderEditor& o, juce::Image img) : owner (o), image (std::move (img))
    {
        setWantsKeyboardFocus (true);
        setOpaque (false);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (getLocalBounds());
        auto r = imageBounds();
        if (! image.isValid() || r.isEmpty())
            return;
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (image, r.toFloat());
        g.setColour (kLine);
        g.drawRect (r, 1);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        owner.hideSplash();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            owner.hideSplash();
            return true;
        }
        return false;
    }

    void inputAttemptWhenModal() override
    {
        owner.hideSplash();
    }

private:
    juce::Rectangle<int> imageBounds() const
    {
        if (! image.isValid())
            return {};
        const int pad = 24;
        auto max = getLocalBounds().reduced (pad);
        const float scale = juce::jmin (1.0f,
                                        (float) max.getWidth() / (float) juce::jmax (1, image.getWidth()),
                                        (float) max.getHeight() / (float) juce::jmax (1, image.getHeight()));
        const int w = juce::jmax (1, juce::roundToInt ((float) image.getWidth() * scale));
        const int h = juce::jmax (1, juce::roundToInt ((float) image.getHeight() * scale));
        return getLocalBounds().withSizeKeepingCentre (w, h);
    }

    ModloaderEditor& owner;
    juce::Image image;
};

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

void StepButton::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::TextButton::mouseDown (e);
}

void StepButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        juce::TextButton::mouseUp (e);
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
    setColour (juce::PopupMenu::backgroundColourId, kPanel);
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::PopupMenu::headerTextColourId, kMuted);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kAmber);
    setColour (juce::PopupMenu::highlightedTextColourId, kBg);
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

int ModloaderLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    if (slider.getSliderStyle() == juce::Slider::LinearVertical)
        return 8;
    return LookAndFeel_V4::getSliderThumbRadius (slider);
}

void ModloaderLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float minSliderPos, float maxSliderPos,
                                            juce::Slider::SliderStyle, juce::Slider& slider)
{
    juce::ignoreUnused (minSliderPos, maxSliderPos);
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    const bool lit = slider.isEnabled() && slider.isMouseOverOrDragging();

    constexpr float capW = 18.0f;
    constexpr float capH = 16.0f;
    constexpr float grooveW = 8.0f;
    const float faderColW = (float) kFaderColW;
    const float cx = bounds.getX() + faderColW * 0.5f;

    auto groove = juce::Rectangle<float> (cx - grooveW * 0.5f, bounds.getY() + 1.0f,
                                          grooveW, bounds.getHeight() - 2.0f);
    g.setColour (kWell);
    g.fillRoundedRectangle (groove, 2.5f);
    g.setColour (kLine);
    g.drawRoundedRectangle (groove.reduced (0.5f), 2.5f, 1.2f);

    auto inner = groove.reduced (1.6f);
    const float fillTop = juce::jlimit (inner.getY(), inner.getBottom(), sliderPos);
    auto fill = inner.withTop (fillTop);
    if (fill.getHeight() > 0.5f)
    {
        g.setColour (kAmber);
        g.fillRoundedRectangle (fill, 1.2f);
    }

    const float thumbR = (float) getSliderThumbRadius (slider);
    const float yTop = (float) y + thumbR;
    const float yBot = (float) (y + height) - thumbR;
    auto yAtDb = [yTop, yBot] (float db)
    {
        // 0 dB at the top; -24 sits ~80% down, matching a mixer scale with a little throw below.
        const float t = juce::jlimit (0.0f, 1.0f, -db / 30.0f);
        return yTop + t * (yBot - yTop);
    };

    const float tickX = bounds.getX() + faderColW + 2.0f;
    constexpr float tickW = 5.0f;
    const auto scaleC = kMuted.brighter (0.35f);
    g.setColour (scaleC);
    for (int n = 0; n <= 30; ++n)
        g.fillRect (tickX, yAtDb ((float) -n) - 0.5f, tickW, 1.0f);

    g.setFont (juce::Font { juce::FontOptions (10.0f) });
    const float labelX = tickX + tickW + 3.0f;
    const float labelW = bounds.getRight() - labelX - 1.0f;
    constexpr float labelH = 14.0f;
    const char* labels[] = { "0 dB", "-6", "-12", "-18", "-24" };
    const int majors[] = { 0, 6, 12, 18, 24 };
    for (int i = 0; i < 5; ++i)
        g.drawText (labels[i], juce::Rectangle<float> (labelX, yAtDb ((float) -majors[i]) - labelH * 0.5f,
                                                       labelW, labelH),
                    juce::Justification::centredLeft, false);

    auto cap = juce::Rectangle<float> (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
    g.setColour (kSlotB);
    g.fillRoundedRectangle (cap, 2.5f);
    g.setColour (lit ? kAmber : kLine);
    g.drawRoundedRectangle (cap.reduced (0.5f), 2.5f, 1.2f);

    const float gx = cap.getX() + 3.5f;
    const float gw = cap.getWidth() - 7.0f;
    const float cy = cap.getCentreY();
    g.setColour (kMuted);
    g.fillRect (gx, cy - 3.0f, gw, 1.0f);
    g.fillRect (gx, cy + 2.0f, gw, 1.0f);
    g.setColour (kText);
    g.fillRect (gx, cy - 0.5f, gw, 1.2f);
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

juce::Font ModloaderLookAndFeel::getPopupMenuFont()
{
    return withDefaultMetrics (juce::FontOptions (13.0f));
}

void ModloaderLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (kPanel);
    g.setColour (kLine);
    g.drawRect (0, 0, width, height, 1);
}

ModloaderEditor::ModloaderEditor (ModloaderProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      shuffleA (p.apvts, "shuffle", shuffle),
      vol808A (p.apvts, "mix_808", vol808),
      vol909A (p.apvts, "mix_909", vol909),
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
    addLabel (songName, "unsaved", kMuted, 14.0f);
    addLabel (status, {}, kMuted, 12.0f);
    addLabel (shuffleN, "SHUFFLE", kMuted, 11.0f);
    shuffleN.setJustificationType (juce::Justification::centredRight);
    addLabel (mixMuteN, "MUTE", kMuted, 11.0f);
    mixMuteN.setJustificationType (juce::Justification::centredRight);
    mixMuteN.setBorderSize ({});
    mixMuteN.setTooltip ("Omit the focused machine from the main mix");
    addLabel (runN, "RUN", kMuted, 11.0f);
    runN.setJustificationType (juce::Justification::centredRight);
    runN.setBorderSize ({});
    runN.setTooltip ("Run the focused machine's sequencer. Turn off when MIDI on a track is playing the drums");
    addLabel (flamN, "FLAM", kMuted, 11.0f);
    flamN.setJustificationType (juce::Justification::centredLeft);
    modName.setJustificationType (juce::Justification::centredLeft);
    modName.setColour (juce::Label::backgroundColourId, kWell);
    modName.setColour (juce::Label::outlineColourId, kLine);
    modName.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    songName.setJustificationType (juce::Justification::centredLeft);
    songName.setColour (juce::Label::backgroundColourId, kWell);
    songName.setColour (juce::Label::outlineColourId, kLine);
    songName.setColour (juce::Label::textColourId, kMuted);
    status.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (modUp);
    addAndMakeVisible (modDown);
    addAndMakeVisible (songOpenBtn);
    addAndMakeVisible (songSaveBtn);
    addAndMakeVisible (settingsBtn);
    songOpenBtn.setTooltip ("Open song");
    songSaveBtn.setTooltip ("Save song");
    settingsBtn.setTooltip ("Settings");
    modDown.onClick = [this] { loadMod (-1); };
    modUp.onClick = [this] { loadMod (1); };
    songOpenBtn.onClick = [this] { chooseSongOpen(); };
    songSaveBtn.onClick = [this] { chooseSongSave(); };
    settingsBtn.onClick = [this] { toggleSettings(); };

    setupKnob (shuffle, false);
    setupFader (vol808);
    setupFader (vol909);
    vol808.setTooltip ("808 Volume");
    vol909.setTooltip ("909 Volume");
    shuffleOn.setClickingTogglesState (true);
    shuffleOn.setButtonText ({});
    shuffleOn.setColour (juce::ToggleButton::textColourId, kMuted);
    addAndMakeVisible (shuffleOn);
    shuffleOn.onClick = [this]
    {
        if (focusedMachine < 0)
            return;
        if (auto* e = eng())
            orb_modloader_set_pat_shuffle (e, focusedMachine, currentPattern (focusedMachine),
                                           shuffleOn.getToggleState() ? 1 : 0);
    };
    mixMuteOn.setClickingTogglesState (true);
    mixMuteOn.setButtonText ({});
    mixMuteOn.setColour (juce::ToggleButton::textColourId, kMuted);
    mixMuteOn.setTooltip ("Omit the focused machine from the main mix");
    addAndMakeVisible (mixMuteOn);
    mixMuteOn.onClick = [this] { setMixMute (mixMuteOn.getToggleState()); };
    runOn.setClickingTogglesState (true);
    runOn.setButtonText ({});
    runOn.setColour (juce::ToggleButton::textColourId, kMuted);
    runOn.setTooltip ("Run the focused machine's sequencer. Turn off when MIDI on a track is playing the drums");
    addAndMakeVisible (runOn);
    runOn.onClick = [this] { setRun (runOn.getToggleState()); };
    midiDrag.onStartDrag = [this] { startPatternMidiDrag(); };
    midiDrag.setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    midiDrag.setRepaintsOnMouseActivity (true);
    addAndMakeVisible (midiDrag);

    setupPatternSection (0);
    setupPatternSection (1);
    setup808();
    setup909();
    setupDisable (disable808, 0);
    setupDisable (disable909, 1);
    addAndMakeVisible (fx808);
    addAndMakeVisible (fx909);
    fx808.setTooltip ("This song used effects on the 808");
    fx909.setTooltip ("This song used effects on the 909");
    fx808.onClick = [this] { toggleEffectsRecall (0); };
    fx909.onClick = [this] { toggleEffectsRecall (1); };
    select808 (1);
    select909 (1);
    addMouseListener (&focusListener, true);
    pat808.shownBank = currentPattern (0) / 8;
    pat909.shownBank = currentPattern (1) / 8;
    refreshPatternHeader();
    refreshMidiDrag();
    refreshModLabel();
    refreshSongLabel();
    refreshSplash();
    refreshSteps();
    refreshFxBadges();
    resized();
    startTimerHz (30);

    juce::Component::SafePointer<ModloaderEditor> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->promptForDatIfNeeded();
    });
}

ModloaderEditor::~ModloaderEditor()
{
    juce::PopupMenu::dismissAllActiveMenus();
    hideModPicker();
    hideSettings();
    hideSplash();
    hideEffectsRecall();
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

void ModloaderEditor::setupFader (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    s.setSliderSnapsToMousePosition (true);
    s.getProperties().set ("fader", true);
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
    {
        refreshShuffle();
        refreshMixMute();
        refreshRun();
        return;
    }
    for (int machine = 0; machine < 2; ++machine)
    {
        auto& ui = machine == 0 ? pat808 : pat909;
        const auto pat = currentPattern (machine);
        for (int i = 0; i < 4; ++i)
            ui.bank[i].setToggleState (i == ui.shownBank, juce::dontSendNotification);
        for (int i = 0; i < 8; ++i)
            ui.pat[i].setToggleState (ui.shownBank == pat / 8 && i == (pat % 8), juce::dontSendNotification);
        ui.stepsValue.setText (juce::String ((int) orb_modloader_get_pat_length (e, machine, pat)),
                               juce::dontSendNotification);
    }
    refreshShuffle();
    refreshMixMute();
    refreshRun();
}

void ModloaderEditor::refreshShuffle()
{
    auto* e = eng();
    const bool have = e != nullptr && focusedMachine >= 0;
    shuffleOn.setEnabled (have);
    const bool on = have
                    && orb_modloader_get_pat_shuffle (e, focusedMachine, currentPattern (focusedMachine)) != 0;
    shuffleOn.setToggleState (on, juce::dontSendNotification);
}

void ModloaderEditor::refreshMixMute()
{
    const bool have = focusedMachine >= 0;
    mixMuteOn.setEnabled (have);
    mixMuteN.setEnabled (have);
    mixMuteN.setColour (juce::Label::textColourId, have ? kMuted : kMuted.withAlpha (0.5f));
    bool on = false;
    if (have)
        if (auto* v = proc.apvts.getRawParameterValue (ModloaderProcessor::mixDirectId (focusedMachine)))
            on = v->load() >= 0.5f;
    mixMuteOn.setToggleState (on, juce::dontSendNotification);
}

void ModloaderEditor::refreshRun()
{
    const bool have = focusedMachine >= 0;
    runOn.setEnabled (have);
    runN.setEnabled (have);
    runN.setColour (juce::Label::textColourId, have ? kMuted : kMuted.withAlpha (0.5f));
    bool on = false;
    if (have)
    {
        const auto id = focusedMachine == 0 ? ModloaderProcessor::p808Id (0)
                                            : ModloaderProcessor::p909Id (0);
        if (auto* v = proc.apvts.getRawParameterValue (id))
            on = v->load() >= 0.5f;
    }
    runOn.setToggleState (on, juce::dontSendNotification);
}

void ModloaderEditor::refreshMidiDrag()
{
    const bool have = focusedMachine >= 0;
    midiDrag.setEnabled (have);
    midiDrag.setTooltip (have
        ? (focusedMachine == 0 ? "Drag 808 pattern to a sequencer track"
                               : "Drag 909 pattern to a sequencer track")
        : "Focus a machine, then drag its pattern to a sequencer track");
}

void ModloaderEditor::startPatternMidiDrag()
{
    if (focusedMachine < 0)
        return;
    uint8_t pat[kPatLen];
    if (! loadFocusedPattern (pat))
        return;
    uint8_t shuffleAmount = 64;
    if (auto* v = proc.apvts.getRawParameterValue ("shuffle"))
        shuffleAmount = (uint8_t) juce::jlimit (0, 127, juce::roundToInt (v->load()));
    const auto name = focusedMachine == 0 ? "OpenRB-Modloader-808.mid"
                                          : "OpenRB-Modloader-909.mid";
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    if (! patternmidi::writeFile (file, focusedMachine, pat, shuffleAmount))
        return;
    performExternalDragDropOfFiles ({ file.getFullPathName() }, false);
}

void ModloaderEditor::setMixMute (bool on)
{
    if (focusedMachine < 0)
        return;
    if (auto* param = proc.apvts.getParameter (ModloaderProcessor::mixDirectId (focusedMachine)))
        param->setValueNotifyingHost (on ? 1.0f : 0.0f);
    refreshMixMute();
    repaint();
}

void ModloaderEditor::setRun (bool on)
{
    if (focusedMachine < 0)
        return;
    const auto id = focusedMachine == 0 ? ModloaderProcessor::p808Id (0)
                                        : ModloaderProcessor::p909Id (0);
    if (auto* param = proc.apvts.getParameter (id))
        param->setValueNotifyingHost (on ? 1.0f : 0.0f);
    refreshRun();
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
    const int index = orb_modloader_current_mod (e);
    orb_modloader_mod_name (e, index, buf, (int) sizeof buf);
    modName.setText (juce::String::fromUTF8 (buf), juce::dontSendNotification);
    orb_modloader_status (e, buf, (int) sizeof buf);
    status.setText (juce::String::fromUTF8 (buf), juce::dontSendNotification);
    settingsBtn.setAttention (statusNeedsDat());
    if (modPicker != nullptr)
        modPicker->syncSelection (index);
}

void ModloaderEditor::refreshSongLabel()
{
    const auto name = proc.songDisplayName();
    const bool unsaved = name == "unsaved";
    songName.setText (name, juce::dontSendNotification);
    songName.setColour (juce::Label::textColourId, unsaved ? kMuted : kText);
    const auto path = proc.currentSong().getFullPathName();
    songName.setTooltip (path);
}

void ModloaderEditor::refreshSplash()
{
    splashImage = {};
    auto* e = eng();
    splashMod = e != nullptr ? orb_modloader_current_mod (e) : -1;
    if (e == nullptr)
        return;
    const int n = orb_modloader_splash_jpeg (e, nullptr, 0);
    if (n <= 0)
        return;
    juce::HeapBlock<uint8_t> buf ((size_t) n);
    if (orb_modloader_splash_jpeg (e, buf.getData(), n) <= 0)
        return;
    splashImage = juce::ImageFileFormat::loadFrom (buf.getData(), (size_t) n);
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
    loadModIndex (i);
}

void ModloaderEditor::loadModIndex (int index)
{
    auto* e = eng();
    if (e == nullptr)
        return;
    const int n = orb_modloader_mod_count (e);
    if (n <= 0)
        return;
    orb_modloader_load (e, juce::jlimit (0, n - 1, index));
    refreshModLabel();
    refreshSplash();
    if (statusNeedsDat())
        showSettings (true);
}

void ModloaderEditor::handleNestedMouseDown (const juce::MouseEvent& e)
{
    const bool onPicker = modPicker != nullptr
        && (e.eventComponent == modPicker.get() || modPicker->isParentOf (e.eventComponent));
    if (! onPicker)
        updateMachineFocus (e.getEventRelativeTo (this).getPosition());
    if (! e.mods.isPopupMenu() && e.eventComponent == &modName)
        toggleModPicker();
    if (! e.mods.isPopupMenu() && e.eventComponent == &mixMuteN && mixMuteN.isEnabled())
        setMixMute (! mixMuteOn.getToggleState());
    if (! e.mods.isPopupMenu() && e.eventComponent == &runN && runN.isEnabled())
        setRun (! runOn.getToggleState());
}

void ModloaderEditor::toggleModPicker()
{
    if (modPicker != nullptr)
        hideModPicker();
    else
        showModPicker();
}

void ModloaderEditor::showModPicker()
{
    auto* e = eng();
    if (e == nullptr || modPicker != nullptr)
        return;
    const int n = orb_modloader_mod_count (e);
    if (n <= 0)
        return;

    juce::StringArray names;
    names.ensureStorageAllocated (n);
    char buf[256] {};
    for (int i = 0; i < n; ++i)
    {
        orb_modloader_mod_name (e, i, buf, (int) sizeof buf);
        names.add (juce::String::fromUTF8 (buf));
    }

    modPicker = std::make_unique<ModPicker> (*this);
    modPicker->setMods (std::move (names), orb_modloader_current_mod (e));

    const int width = modPicker->preferredWidth (modName.getWidth());
    const int maxH = juce::jmax (24, getHeight() - modName.getBottom() - 8);
    const int height = modPicker->preferredHeight (juce::jmin (maxH, 22 * 14 + 2));
    auto bounds = juce::Rectangle<int> (modName.getX(), modName.getBottom() - 1, width, height);
    if (bounds.getRight() > getWidth() - 8)
        bounds.setX (getWidth() - 8 - bounds.getWidth());
    addAndMakeVisible (*modPicker);
    modPicker->setBounds (bounds);
    modPicker->toFront (true);
    modPicker->enterModalState (true, nullptr, false);
}

void ModloaderEditor::hideModPicker()
{
    if (modPicker == nullptr)
        return;
    auto picker = std::move (modPicker);
    if (picker->isCurrentlyModal())
        picker->exitModalState (0);
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

    hideModPicker();
    refreshModLabel();
    refreshSplash();
    if (statusNeedsDat())
        showSettings (true);
    else if (settingsPanel != nullptr)
        settingsPanel->refresh();
}

void ModloaderEditor::showSongError (const juce::String& heading, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon, heading, message);
}

void ModloaderEditor::finishSongLoad()
{
    const auto mod = proc.loadedSongModName();
    if (mod.isNotEmpty() && proc.loadModByName (mod) < 0)
        showSongError ("Mod not found",
                       "This song uses the mod \"" + mod + "\", which was not in the Mods folder.");

    pat808.shownBank = currentPattern (0) / 8;
    pat909.shownBank = currentPattern (1) / 8;
    refreshPatternHeader();
    refreshSteps();
    refreshSongLabel();
    refreshModLabel();
    refreshSplash();
    refreshDisable();
    sync808Radios();
    refreshFxBadges();
    if (statusNeedsDat())
        showSettings (true);
}

void ModloaderEditor::applyOpenedSong (const juce::File& f)
{
    if (f.getFullPathName().isEmpty())
        return;
    juce::String error;
    if (! proc.loadSongFile (f, error))
    {
        showSongError ("Could not open song", error);
        refreshModLabel();
        return;
    }
    finishSongLoad();
}

void ModloaderEditor::chooseSongOpen()
{
    hideModPicker();
    if (fileChooserOpen)
        return;
    juce::File start = proc.currentSong();
    if (! start.existsAsFile())
        start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    fileChooserOpen = true;
    chooser = std::make_unique<juce::FileChooser> (
        "Open ReBirth song", start, "*.rbs;*.RBS");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              fileChooserOpen = false;
                              applyOpenedSong (fc.getResult());
                          });
}

void ModloaderEditor::chooseSongSave()
{
    hideModPicker();
    if (fileChooserOpen)
        return;
    juce::File start = proc.currentSong();
    if (start.getFullPathName().isEmpty())
        start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                    .getChildFile ("Untitled.rbs");

    fileChooserOpen = true;
    chooser = std::make_unique<juce::FileChooser> (
        "Save ReBirth song", start, "*.rbs;*.RBS");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              fileChooserOpen = false;
                              auto f = fc.getResult();
                              if (f.getFullPathName().isEmpty())
                                  return;
                              if (f.getFileExtension().isEmpty())
                                  f = f.withFileExtension ("rbs");
                              juce::String error;
                              if (! proc.saveSongFile (f, error))
                              {
                                  showSongError ("Could not save song", error);
                                  refreshModLabel();
                                  return;
                              }
                              refreshSongLabel();
                          });
}

void ModloaderEditor::chooseModsFolder()
{
    hideModPicker();
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

    fileChooserOpen = true;
    chooser = std::make_unique<juce::FileChooser> (
        "ReBirth mod (.rbm) or Mods folder", start, "*.rbm;*.RBM");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              fileChooserOpen = false;
                              applyPickedMod (fc.getResult());
                          });
}

void ModloaderEditor::chooseDat()
{
    hideModPicker();
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

    fileChooserOpen = true;
    chooser = std::make_unique<juce::FileChooser> (
        "RB20FUL.DAT or ReBirth 2.x CD image", start, "*.DAT;*.dat;*.iso;*.ISO;*.cdr;*.CDR");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              fileChooserOpen = false;
                              applyDatFile (fc.getResult());
                          });
}

void ModloaderEditor::applyDatFile (const juce::File& f)
{
    auto* engine = eng();
    if (! f.existsAsFile() || engine == nullptr)
        return;
    proc.setKeyPath (f.getFullPathName());
    orb_modloader_load (engine, orb_modloader_current_mod (engine));
    refreshModLabel();
    refreshSplash();
    if (settingsPanel != nullptr)
        settingsPanel->setLocateDat (statusNeedsDat());
}

void ModloaderEditor::toggleSettings()
{
    if (settingsPanel != nullptr)
        hideSettings();
    else
        showSettings (statusNeedsDat());
}

void ModloaderEditor::showSettings (bool locateDat)
{
    hideModPicker();
    hideSplash();
    hideEffectsRecall();
    if (settingsPanel != nullptr)
    {
        settingsPanel->setLocateDat (locateDat);
        return;
    }
    juce::Component::SafePointer<ModloaderEditor> safe (this);
    juce::MessageManager::callAsync ([safe, locateDat]
    {
        if (safe != nullptr)
            safe->presentSettings (locateDat);
    });
}

void ModloaderEditor::presentSettings (bool locateDat)
{
    if (settingsPanel != nullptr)
    {
        settingsPanel->setLocateDat (locateDat);
        return;
    }
    settingsPanel = std::make_unique<SettingsPanel> (*this, locateDat);
    addAndMakeVisible (*settingsPanel);
    settingsPanel->setBounds (getLocalBounds());
    settingsPanel->toFront (true);
    settingsPanel->enterModalState (true, nullptr, false);
    settingsPanel->grabKeyboardFocus();
}

void ModloaderEditor::hideSettings()
{
    if (settingsPanel == nullptr)
        return;
    auto panel = std::move (settingsPanel);
    if (panel->isCurrentlyModal())
        panel->exitModalState (0);
}

void ModloaderEditor::showSplash()
{
    hideModPicker();
    hideSettings();
    hideEffectsRecall();
    if (splashViewer != nullptr || ! splashImage.isValid())
        return;
    splashViewer = std::make_unique<SplashViewer> (*this, splashImage);
    addAndMakeVisible (*splashViewer);
    splashViewer->setBounds (getLocalBounds());
    splashViewer->toFront (true);
    splashViewer->enterModalState (true, nullptr, false);
    splashViewer->grabKeyboardFocus();
}

void ModloaderEditor::hideSplash()
{
    if (splashViewer == nullptr)
        return;
    auto viewer = std::move (splashViewer);
    if (viewer->isCurrentlyModal())
        viewer->exitModalState (0);
}

void ModloaderEditor::refreshFxBadges()
{
    fx808.setVisible (proc.machineHasEffects (0));
    fx909.setVisible (proc.machineHasEffects (1));
    if (effectsPanel != nullptr && ! proc.machineHasEffects (effectsPanel->currentMachine()))
        hideEffectsRecall();
}

void ModloaderEditor::toggleEffectsRecall (int machine)
{
    if (effectsPanel != nullptr && effectsPanel->currentMachine() == machine)
        hideEffectsRecall();
    else
        showEffectsRecall (machine);
}

void ModloaderEditor::showEffectsRecall (int machine)
{
    hideModPicker();
    hideSettings();
    hideSplash();
    if (effectsPanel != nullptr)
    {
        effectsPanel->setMachine (machine);
        return;
    }
    juce::Component::SafePointer<ModloaderEditor> safe (this);
    juce::MessageManager::callAsync ([safe, machine]
    {
        if (safe != nullptr)
            safe->presentEffectsRecall (machine);
    });
}

void ModloaderEditor::presentEffectsRecall (int machine)
{
    if (effectsPanel != nullptr)
    {
        effectsPanel->setMachine (machine);
        return;
    }
    if (! proc.machineHasEffects (machine))
        return;
    effectsPanel = std::make_unique<EffectsRecallPanel> (*this, machine);
    addAndMakeVisible (*effectsPanel);
    effectsPanel->setBounds (getLocalBounds());
    effectsPanel->toFront (true);
    effectsPanel->enterModalState (true, nullptr, false);
    effectsPanel->grabKeyboardFocus();
}

void ModloaderEditor::hideEffectsRecall()
{
    if (effectsPanel == nullptr)
        return;
    auto panel = std::move (effectsPanel);
    if (panel->isCurrentlyModal())
        panel->exitModalState (0);
}

bool ModloaderEditor::statusNeedsDat() const
{
    auto* e = proc.engine();
    if (e == nullptr)
        return false;
    char buf[256] {};
    orb_modloader_status (e, buf, (int) sizeof buf);
    return juce::String::fromUTF8 (buf) == "needs DAT/ISO";
}

juce::String ModloaderEditor::datLocateMessage() const
{
    auto* e = proc.engine();
    char path[1024] {};
    if (e != nullptr)
        orb_modloader_dat_path (e, path, (int) sizeof path);
    const juce::File previous (juce::String::fromUTF8 (path));
    if (path[0] != 0 && ! previous.existsAsFile())
        return "The DAT/ISO file is missing from its previous location.\nLocate RB20FUL.DAT or a ReBirth 2.x CD image.";
    return "This mod requires RB20FUL.DAT or a ReBirth 2.x CD image.\nLocate the file to continue.";
}

void ModloaderEditor::promptForDatIfNeeded()
{
    if (statusNeedsDat())
        showSettings (true);
}

int ModloaderEditor::focusedDrum() const
{
    return focusedMachine == 0 ? sel808 : sel909;
}

bool ModloaderEditor::loadFocusedPattern (uint8_t* dest)
{
    auto* e = eng();
    if (e == nullptr || focusedMachine < 0)
        return false;
    uint8_t all[ModloaderProcessor::kPatternBytes];
    if (orb_modloader_get_patterns (e, focusedMachine, all, ModloaderProcessor::kPatternBytes) <= 0)
        return false;
    std::memcpy (dest, all + (size_t) currentPattern (focusedMachine) * (size_t) kPatLen, (size_t) kPatLen);
    return true;
}

void ModloaderEditor::storeFocusedPattern (const uint8_t* src)
{
    auto* e = eng();
    if (e == nullptr || focusedMachine < 0)
        return;
    uint8_t all[ModloaderProcessor::kPatternBytes];
    if (orb_modloader_get_patterns (e, focusedMachine, all, ModloaderProcessor::kPatternBytes) <= 0)
        return;
    std::memcpy (all + (size_t) currentPattern (focusedMachine) * (size_t) kPatLen, src, (size_t) kPatLen);
    orb_modloader_set_patterns (e, focusedMachine, all, ModloaderProcessor::kPatternBytes);
    refreshSteps();
    refreshPatternHeader();
}

void ModloaderEditor::resetDevice()
{
    proc.resetDevice();
    flamEntry.setToggleState (false, juce::dontSendNotification);
    pat808.shownBank = 0;
    pat909.shownBank = 0;
    select808 (1);
    select909 (1);
    refreshPatternHeader();
    refreshSteps();
    refreshSongLabel();
    refreshDisable();
    sync808Radios();
    refreshFxBadges();
    hideEffectsRecall();
    repaint();
}

void ModloaderEditor::editCut()
{
    editCopy();
    editClear();
}

void ModloaderEditor::editCopy()
{
    uint8_t pat[kPatLen];
    if (! loadFocusedPattern (pat))
        return;
    proc.patternClip.valid = true;
    proc.patternClip.machine = focusedMachine;
    std::memcpy (proc.patternClip.bytes.data(), pat, (size_t) kPatLen);
}

void ModloaderEditor::editPaste (bool replace)
{
    if (! proc.patternClip.valid || proc.patternClip.machine != focusedMachine)
        return;
    uint8_t pat[kPatLen];
    if (replace)
    {
        std::memcpy (pat, proc.patternClip.bytes.data(), (size_t) kPatLen);
        storeFocusedPattern (pat);
        return;
    }
    if (! loadFocusedPattern (pat))
        return;
    for (int s = 0; s < kSteps; ++s)
        for (int c = 0; c < kCols; ++c)
            if (const auto v = proc.patternClip.bytes[(size_t) (2 + kCols * s + c)]; v != 0)
                patCell (pat, s, c) = v;
    if (focusedMachine == 1)
        exclusiveHats (pat, k909OH);
    storeFocusedPattern (pat);
}

void ModloaderEditor::editClear()
{
    uint8_t pat[kPatLen];
    if (! loadFocusedPattern (pat))
        return;
    clearSteps (pat);
    storeFocusedPattern (pat);
}

void ModloaderEditor::editShift (bool right, bool drumOnly)
{
    uint8_t pat[kPatLen];
    if (! loadFocusedPattern (pat))
        return;
    if (drumOnly)
    {
        const int col = focusedDrum();
        shiftColumn (pat, col, right);
        if (focusedMachine == 1 && (col == k909CH || col == k909OH))
            exclusiveHats (pat, col);
    }
    else
    {
        for (int c = 0; c < kCols; ++c)
            shiftColumn (pat, c, right);
    }
    storeFocusedPattern (pat);
}

void ModloaderEditor::editRandomize (bool drumOnly)
{
    uint8_t pat[kPatLen];
    if (! loadFocusedPattern (pat))
        return;
    juce::Random rng;
    const bool is909 = focusedMachine == 1;
    if (drumOnly)
    {
        const int col = focusedDrum();
        for (int s = 0; s < kSteps; ++s)
            patCell (pat, s, col) = randomCell (is909, col, rng);
        if (is909 && (col == k909CH || col == k909OH))
            exclusiveHats (pat, col);
    }
    else
    {
        for (int c = 0; c < kCols; ++c)
            for (int s = 0; s < kSteps; ++s)
                patCell (pat, s, c) = randomCell (is909, c, rng);
        if (is909)
            exclusiveHats (pat, k909OH);
    }
    storeFocusedPattern (pat);
}

void ModloaderEditor::editAlter (bool drumOnly)
{
    uint8_t pat[kPatLen];
    if (! loadFocusedPattern (pat))
        return;
    juce::Random rng;
    if (drumOnly)
    {
        const int col = focusedDrum();
        shuffleColumn (pat, col, rng);
        if (focusedMachine == 1 && (col == k909CH || col == k909OH))
            exclusiveHats (pat, col);
    }
    else
    {
        for (int c = 0; c < kCols; ++c)
            shuffleColumn (pat, c, rng);
        if (focusedMachine == 1)
            exclusiveHats (pat, k909OH);
    }
    storeFocusedPattern (pat);
}

void ModloaderEditor::showContextMenu()
{
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return;

    const bool haveFocus = focusedMachine >= 0;
    const bool canPaste = haveFocus && proc.patternClip.valid
                          && proc.patternClip.machine == focusedMachine;

    juce::PopupMenu edit;
    edit.setLookAndFeel (&lnf);
    edit.addItem ("Cut Pattern", haveFocus, false, [this] { editCut(); });
    edit.addItem ("Copy Pattern", haveFocus, false, [this] { editCopy(); });
    edit.addItem ("Paste Pattern", canPaste, false, [this] { editPaste (false); });
    edit.addItem ("Paste Replace", canPaste, false, [this] { editPaste (true); });
    edit.addItem ("Clear Pattern", haveFocus, false, [this] { editClear(); });
    edit.addSeparator();
    edit.addItem ("Shift Pattern Left", haveFocus, false, [this] { editShift (false, false); });
    edit.addItem ("Shift Pattern Right", haveFocus, false, [this] { editShift (true, false); });
    edit.addItem ("Shift Drum Left", haveFocus, false, [this] { editShift (false, true); });
    edit.addItem ("Shift Drum Right", haveFocus, false, [this] { editShift (true, true); });
    edit.addSeparator();
    edit.addItem ("Randomize Pattern", haveFocus, false, [this] { editRandomize (false); });
    edit.addItem ("Randomize Drum", haveFocus, false, [this] { editRandomize (true); });
    edit.addItem ("Alter Pattern", haveFocus, false, [this] { editAlter (false); });
    edit.addItem ("Alter Drum", haveFocus, false, [this] { editAlter (true); });

    juce::PopupMenu menu;
    menu.setLookAndFeel (&lnf);
    menu.addItem ("Reset Device", [this] { resetDevice(); });
    menu.addSubMenu ("Edit", std::move (edit));
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withParentComponent (this)
                            .withMousePosition());
}

void ModloaderEditor::timerCallback()
{
    refreshSteps();
    refreshPatternHeader();
    refreshDisable();
    sync808Radios();
    refreshModLabel();
    refreshFxBadges();
    if (auto* e = eng())
        if (orb_modloader_current_mod (e) != splashMod)
            refreshSplash();
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
    refreshShuffle();
    refreshMixMute();
    refreshRun();
    refreshMidiDrag();
    repaint();
}

void ModloaderEditor::updateSplashCursor (juce::Point<int> pos)
{
    const bool over = splashImage.isValid() && splash808.contains (pos);
    setMouseCursor (over ? juce::MouseCursor::PointingHandCursor
                         : juce::MouseCursor::NormalCursor);
}

void ModloaderEditor::mouseMove (const juce::MouseEvent& e)
{
    updateSplashCursor (e.getPosition());
}

void ModloaderEditor::mouseExit (const juce::MouseEvent&)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ModloaderEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getEventRelativeTo (this).getPosition();
    if (! e.mods.isPopupMenu() && splashImage.isValid() && splash808.contains (pos))
    {
        showSplash();
        return;
    }
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

    auto paintEdgeBar = [&] (juce::Rectangle<int> machine, bool left, juce::Colour colour)
    {
        const auto r = machine.toFloat();
        const float x = left ? r.getX() + 3.0f : r.getRight() - 5.0f;
        g.setColour (colour);
        g.fillRect (x, r.getY() + 10.0f, 2.0f, r.getHeight() - 20.0f);
    };
    if (focusedMachine >= 0)
        paintEdgeBar (focusedMachine == 0 ? kMachine808 : kMachine909, true, kAmber);

    auto machineMixMuted = [this] (int machine)
    {
        if (auto* v = proc.apvts.getRawParameterValue (ModloaderProcessor::mixDirectId (machine)))
            return v->load() >= 0.5f;
        return false;
    };
    if (machineMixMuted (0))
        paintEdgeBar (kMachine808, false, kRed);
    if (machineMixMuted (1))
        paintEdgeBar (kMachine909, false, kRed);

    auto paintSlots = [&] (const std::array<juce::Rectangle<int>, 12>& slots, bool joinHats)
    {
        for (int i = 0; i < 12; ++i)
        {
            auto r = slots[(size_t) i].toFloat();
            if (r.isEmpty())
                continue;
            const bool dark = joinHats && i >= 9 ? (i % 2 != 0) : (i % 2 == 0);
            g.setColour (dark ? kSlotA : kSlotB);
            g.fillRect (r);
            g.setColour (kLine);
            if (! (joinHats && i == 9))
                g.fillRect (r.getX(), r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);
            if (i == 11)
                g.fillRect (r.getRight() - 1.0f, r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);
        }
    };
    paintSlots (slot808, false);
    paintSlots (slot909, true);

    auto paintSplash = [&] (juce::Rectangle<int> r)
    {
        if (r.isEmpty() || ! splashImage.isValid())
            return;
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (splashImage, r.toFloat());
        g.setColour (kLine);
        g.drawRect (r, 1);
    };
    paintSplash (splash808);

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
        g.setColour (kRed);
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
    title.setBounds (top.removeFromLeft (178));
    const int bandY = top.getY() + 6;
    const int bandH = top.getHeight() - 12;
    auto icon = [bandY, bandH] (juce::Rectangle<int>& row)
    {
        return row.removeFromLeft (bandH).withY (bandY).withHeight (bandH).reduced (1, 0);
    };
    modName.setBounds (top.removeFromLeft (150).withY (bandY).withHeight (bandH));
    auto arrows = top.removeFromLeft (16).withY (bandY).withHeight (bandH);
    modUp.setBounds (arrows.removeFromTop (arrows.getHeight() / 2).reduced (1, 0));
    modDown.setBounds (arrows.reduced (1, 0));
    top.removeFromLeft (8);
    songName.setBounds (top.removeFromLeft (148).withY (bandY).withHeight (bandH));
    top.removeFromLeft (4);
    songOpenBtn.setBounds (icon (top));
    songSaveBtn.setBounds (icon (top));
    top.removeFromLeft (8);
    settingsBtn.setBounds (icon (top));
    top.removeFromLeft (8);
    shuffle.setBounds (top.removeFromRight (40).reduced (0, 2));
    constexpr int shuffleTick = 22;
    shuffleOn.setBounds (top.removeFromRight (shuffleTick).withSizeKeepingCentre (shuffleTick, shuffleTick));
    shuffleN.setBounds (top.removeFromRight (56));
    top.removeFromRight (30);
    mixMuteOn.setBounds (top.removeFromRight (shuffleTick).withSizeKeepingCentre (shuffleTick, shuffleTick));
    const int muteLabelW = captionLabelWidth ("MUTE") + 2;
    mixMuteN.setBounds (top.removeFromRight (muteLabelW).withSizeKeepingCentre (muteLabelW, 14));
    top.removeFromRight (40);
    midiDrag.setBounds (top.removeFromRight (shuffleTick).withSizeKeepingCentre (shuffleTick, shuffleTick));
    top.removeFromRight (40);
    runOn.setBounds (top.removeFromRight (shuffleTick).withSizeKeepingCentre (shuffleTick, shuffleTick));
    const int runLabelW = captionLabelWidth ("RUN") + 2;
    runN.setBounds (top.removeFromRight (runLabelW).withSizeKeepingCentre (runLabelW, 14));
    status.setBounds (top.withY (bandY).withHeight (bandH));

    auto placePattern = [this] (PatternUi& ui, juce::Rectangle<int> r, bool withFlam, bool withSplash)
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

        if (withSplash)
        {
            r.removeFromTop (kSplashGap);
            auto thumb = r.removeFromTop (kSplashH);
            splash808 = { thumb.getX() + (thumb.getWidth() - kSplashW) / 2, thumb.getY(), kSplashW, kSplashH };
        }

        auto bottom = r.removeFromBottom (withFlam ? 88 : 46);
        if (withFlam)
        {
            auto flamRow = bottom.removeFromTop (36).reduced (1, 0);
            flamEntry.setBounds (flamRow.removeFromLeft (22).reduced (0, 6));
            flamN.setJustificationType (juce::Justification::centredLeft);
            auto flamLabel = flamRow.removeFromLeft (38);
            flamN.setBounds (flamLabel.withSizeKeepingCentre (flamLabel.getWidth(), 18));
            k909[29].setBounds (flamRow.removeFromLeft (kFlamKnob)
                                    .withSizeKeepingCentre (kFlamKnob, kFlamKnob));
            bottom.removeFromTop (6);
        }
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
    placePattern (pat808, { leftColX, 72 + enableH + enableGap, leftColW, 322 - (72 + enableH + enableGap) }, false, true);
    placePattern (pat909, { leftColX, 350 + enableH + enableGap, leftColW, 620 - (350 + enableH + enableGap) }, true, false);

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
        const int x = slot808X (col);
        placeLabeled (k808[(size_t) ctrl], n808[(size_t) ctrl], x, y808 + row * kCtrlRow, k808SlotW[col]);
    };
    auto place808Sw = [this] (int i, int col, int row)
    {
        const int slotLeft = slot808X (col);
        const int slotW = k808SlotW[col];
        const int y = y808 + row * kCtrlRow + 6;
        constexpr int h = 16;
        auto& alt = sw808[(size_t) i];
        auto& def = sw808Def[(size_t) i];
        const int w = juce::jmax (toggleContentWidth (alt, h), toggleContentWidth (def, h));
        const int x = slotLeft + (slotW - w) / 2;
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
            const int x = is808 ? slot808X (i) : slot909X (i);
            const int w = is808 ? k808SlotW[i] : k909SlotW[i];
            slotRects[(size_t) i] = { x, y0 - 6, w, slotBottom - (y0 - 6) };
            int lx = x, lw = w;
            if (! is808 && (i == 8 || i == 9))
            {
                lw = k909PairHalf;
                lx = i == 8 ? x + w - lw : x;
            }
            legends[(size_t) i].setBounds (lx, legendY, lw, legendH);
        }
        for (int i = 0; i < 16; ++i)
        {
            steps[(size_t) i].setBounds (kSlot0X + i * 46, stepY, 42, stepH);
            leds[(size_t) i].setBounds (kSlot0X + i * 46 + 17, ledY, 7, ledH);
        }
        const int muteW = toggleContentWidth (mute, stepH);
        // tickRight draws the box 4px inset from the button's right edge.
        mute.setBounds (slotRects[11].getRight() + 4 - muteW, stepY, muteW, stepH);
    };
    placeSlotsAndSequencer (legend808, step808, led808, disable808, slot808,
                            y808, contentBottom808, true);

    auto place909Knob = [this, &placeLabeled] (int ctrl, int col, int row, SlotSide side)
    {
        const int slotX = slot909X (col);
        const int slotW = k909SlotW[col];
        const int y = y909 + row * kCtrlRow;
        int x = slotX, w = slotW;
        if (col == 8 || col == 9)
        {
            w = k909PairHalf;
            x = col == 8 ? slotX + slotW - w : slotX;
        }
        else if (side != SlotSide::Centre)
        {
            w = (slotW - k909PairPad * 2) / 2;
            x = side == SlotSide::Left ? slotX + k909PairPad : slotX + k909PairPad + w;
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

    auto placeMixFader = [&] (juce::Slider& fader, const std::array<juce::Rectangle<int>, 12>& slots)
    {
        const int availX = kSlot0X + k909SlotAreaW + 6;
        const int availW = machineRight - availX;
        const auto slot = slots[0];
        fader.setBounds (availX, slot.getY(), availW, slot.getHeight());
    };
    placeMixFader (vol808, slot808);
    placeMixFader (vol909, slot909);

    auto placeFxBadge = [&] (FxBadgeButton& btn, const juce::Rectangle<int>& machine)
    {
        const int availX = kSlot0X + k909SlotAreaW + 6;
        const int availW = machineRight - availX;
        constexpr int size = 22;
        const int x = availX + (availW - size) / 2;
        const int y = machine.getBottom() - 24 - size;
        btn.setBounds (x, y, size, size);
    };
    placeFxBadge (fx808, kMachine808);
    placeFxBadge (fx909, kMachine909);

    if (settingsPanel != nullptr)
        settingsPanel->setBounds (getLocalBounds());
    if (splashViewer != nullptr)
        splashViewer->setBounds (getLocalBounds());
    if (effectsPanel != nullptr)
        effectsPanel->setBounds (getLocalBounds());
}
