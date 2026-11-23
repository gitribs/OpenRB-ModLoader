#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PatternMidi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

static_assert (offsetof (OrbModloaderParams, mix808) == 83);
static_assert (offsetof (OrbModloaderParams, mix909) == 84);
static_assert (offsetof (OrbModloaderParams, direct_mix808) == 85);
static_assert (offsetof (OrbModloaderParams, direct_mix909) == 86);
static_assert (offsetof (OrbModloaderParams, mix808_pan) == 87);
static_assert (offsetof (OrbModloaderParams, mix909_pan) == 88);
static_assert (offsetof (OrbModloaderParams, dist808) == 89);
static_assert (offsetof (OrbModloaderParams, dist909) == 90);
static_assert (offsetof (OrbModloaderParams, pcf_id) == 91);
static_assert (sizeof (OrbModloaderParams) == 92);
static_assert (offsetof (OrbModloaderSong, mix808_pan) == 62);
static_assert (offsetof (OrbModloaderSong, dist_enabled) == 129);
static_assert (offsetof (OrbModloaderSong, delay_send_909) == 148);
static_assert (offsetof (OrbModloaderSong, song_mode) == 149);
static_assert (offsetof (OrbModloaderSong, tempo) == 156);
static_assert (sizeof (OrbModloaderSong) == 168);
static_assert (sizeof (OrbModloaderPlayhead) == 24);
static_assert (sizeof (OrbModloaderTrackEvent) == 8);

constexpr uint32_t kTicksPerBar = 768;
constexpr int kTicksPerSixteenth = (int) kTicksPerBar / 16; // 48
constexpr int kTicksPerPos = (int) kTicksPerBar / 32;       // 24, TRAK / 32nd grid

namespace
{
constexpr double kEngineRate = 44100.0;
constexpr int kBlock = 64;

constexpr uint8_t kDefault808[] = {
    1, 0, 40, 100, 64, 64, 100, 64, 64, 100, 64, 0, 100, 64, 0, 100, 64, 0, 100, 0, 100, 0, 100,
    100, 0, 100, 100, 100, 100
};
constexpr uint8_t kDefault909[] = {
    1, 0, 32, 100, 64, 64, 127, 100, 64, 64, 64, 100, 64, 127, 100, 64, 127, 100, 64, 127, 100,
    100, 100, 32, 127, 100, 64, 100, 64, 64
};

const char* kMixNames[ModloaderProcessor::kMachineBuses] = { "808 Mix", "909 Mix" };

const char* kAuxNames[ORB_MODLOADER_AUX] = {
    "808 BD", "808 SD", "808 LT", "808 MT", "808 HT", "808 RS", "808 CP", "808 CB", "808 OH",
    "808 CH", "808 CY", "909 BD", "909 SD", "909 LT", "909 MT", "909 HT", "909 RS", "909 CP",
    "909 CH", "909 OH", "909 CC", "909 RC"
};

const char* k808Names[ORB_MODLOADER_P808] = {
    "808 Enable", "808 Pattern", "808 Accent", "808 BD Level", "808 BD Tone", "808 BD Decay",
    "808 SD Level", "808 SD Tone", "808 SD Snappy", "808 LT Level", "808 LT Tune", "808 LT/LC",
    "808 MT Level", "808 MT Tune", "808 MT/MC", "808 HT Level", "808 HT Tune", "808 HT/HC",
    "808 RS Level", "808 RS/CL", "808 CP Level", "808 CP/MA", "808 CB Level", "808 CY Level",
    "808 CY Tone", "808 CY Decay", "808 OH Level", "808 OH Decay", "808 CH Level"
};

const char* k909Names[ORB_MODLOADER_P909] = {
    "909 Enable", "909 Pattern", "909 Accent", "909 BD Level", "909 BD Tune", "909 BD Attack",
    "909 BD Decay", "909 SD Level", "909 SD Tune", "909 SD Tone", "909 SD Snappy", "909 LT Level",
    "909 LT Tune", "909 LT Decay", "909 MT Level", "909 MT Tune", "909 MT Decay", "909 HT Level",
    "909 HT Tune", "909 HT Decay", "909 RS Level", "909 CP Level", "909 HH Level", "909 CH Decay",
    "909 OH Decay", "909 CC Level", "909 CC Tune", "909 RC Level", "909 RC Tune", "909 Flam"
};

bool ratesMatch (double a, double b)
{
    return std::abs (a - b) < 1.0e-6;
}

bool isSwitch808 (int ctrl)
{
    return ctrl == 11 || ctrl == 14 || ctrl == 17 || ctrl == 19 || ctrl == 21;
}

juce::StringArray patternChoiceNames()
{
    juce::StringArray names;
    names.ensureStorageAllocated (32);
    for (int i = 0; i < 32; ++i)
        names.add (juce::String::charToString ((juce::juce_wchar) ('A' + i / 8))
                   + juce::String ((i % 8) + 1));
    return names;
}

void fillBusFrom (juce::AudioBuffer<float>&& bus, const float* src, int n)
{
    if (src == nullptr || n <= 0)
        return;
    for (int ch = 0; ch < bus.getNumChannels(); ++ch)
    {
        float* dst = bus.getWritePointer (ch);
        if (dst != nullptr && dst != src)
            std::memcpy (dst, src, (size_t) n * sizeof (float));
    }
}

void copyMono (float* dst, const float* src, int n)
{
    if (dst == nullptr || src == nullptr || dst == src || n <= 0)
        return;
    std::memcpy (dst, src, (size_t) n * sizeof (float));
}

void applyMainMute (float* mainL, float* mainR, const float* mix808L, const float* mix808R,
                    const float* mix909L, const float* mix909R,
                    bool mute808, bool mute909, int n)
{
    if (n <= 0 || mainL == nullptr)
        return;
    if (mute808 && mute909)
    {
        juce::FloatVectorOperations::clear (mainL, n);
        if (mainR != nullptr && mainR != mainL)
            juce::FloatVectorOperations::clear (mainR, n);
        return;
    }
    if (! mute808 && ! mute909)
        return;
    const float* srcL = mute909 ? mix808L : mix909L;
    const float* srcR = mute909 ? mix808R : mix909R;
    copyMono (mainL, srcL, n);
    copyMono (mainR, srcR != nullptr ? srcR : srcL, n);
}

void copySongFx (OrbModloaderSong& dest, const OrbModloaderSong& src)
{
    dest.dist_enabled = src.dist_enabled;
    dest.dist_amount = src.dist_amount;
    dest.dist_shape = src.dist_shape;
    dest.dist_808 = src.dist_808;
    dest.dist_909 = src.dist_909;
    dest.pcf_enabled = src.pcf_enabled;
    dest.pcf_frequency = src.pcf_frequency;
    dest.pcf_resonance = src.pcf_resonance;
    dest.pcf_amount = src.pcf_amount;
    dest.pcf_wave = src.pcf_wave;
    dest.pcf_decay = src.pcf_decay;
    dest.pcf_mode = src.pcf_mode;
    dest.pcf_id = src.pcf_id;
    dest.delay_enabled = src.delay_enabled;
    dest.delay_steps = src.delay_steps;
    dest.delay_mode = src.delay_mode;
    dest.delay_feedback = src.delay_feedback;
    dest.delay_pan = src.delay_pan;
    dest.delay_send_808 = src.delay_send_808;
    dest.delay_send_909 = src.delay_send_909;
    dest.reserved[0] = src.reserved[0];
}

uint8_t fxByte (const juce::XmlElement& xml, const char* name)
{
    return (uint8_t) juce::jlimit (0, 255, xml.getIntAttribute (name, 0));
}

constexpr int kFxPrintPpq = 192;
constexpr uint8_t kMixPcfId = 2;
constexpr uint8_t kMixDelaySend808 = 20;
constexpr uint8_t kMixDist808 = 21;
constexpr uint8_t kMixDelaySend909 = 26;
constexpr uint8_t kMixDist909 = 27;
constexpr int kFxParamCap = 8;

struct FxPrintLine
{
    int tick = 0;
    int order = 0;
    const char* name = nullptr;
    int value = 0;
};

std::vector<OrbModloaderTrackEvent> copyTrackEvents (OrbModloader* m, int32_t track)
{
    const int n = orb_modloader_copy_track_events (m, track, nullptr, 0);
    std::vector<OrbModloaderTrackEvent> events ((size_t) juce::jmax (0, n));
    if (n > 0)
        orb_modloader_copy_track_events (m, track, events.data(), n);
    return events;
}

int pcfModeToPlugin (uint8_t value)
{
    return value == 1 ? 0 : 1;
}

int tickToStartBar (int tick)
{
    return juce::jmax (1, tick / (int) kTicksPerBar + 1);
}

int firstCtrlTick (const std::vector<OrbModloaderTrackEvent>& events, uint8_t ctrl,
                   bool nonzero, uint8_t value)
{
    int found = -1;
    for (const auto& e : events)
    {
        if (e.ctrl != ctrl)
            continue;
        if (nonzero ? e.value == 0 : e.value != value)
            continue;
        if (found < 0 || e.tick < found)
            found = e.tick;
    }
    return found;
}

void appendMappedCtrl (std::vector<FxPrintLine>& out,
                       const std::vector<OrbModloaderTrackEvent>& events,
                       uint8_t ctrl, int order, const char* name,
                       int (*convert) (uint8_t))
{
    for (const auto& e : events)
    {
        if (e.ctrl != ctrl)
            continue;
        const int value = convert != nullptr ? convert (e.value) : (int) e.value;
        out.push_back ({ e.tick, order, name, value });
    }
}

uint8_t lastCtrlAtOrBefore (const std::vector<OrbModloaderTrackEvent>& events, uint8_t ctrl,
                            int tick, uint8_t fallback)
{
    uint8_t value = fallback;
    for (const auto& e : events)
    {
        if (e.ctrl != ctrl || e.tick > tick)
            continue;
        value = e.value;
    }
    return value;
}

void appendEnableTimeline (std::vector<FxPrintLine>& out, int order,
                           const std::vector<OrbModloaderTrackEvent>& device, uint8_t deviceCtrl,
                           const std::vector<OrbModloaderTrackEvent>* mix, int mixCtrl, int mixMatch,
                           uint8_t fallbackDev, uint8_t fallbackMix)
{
    uint8_t dev = lastCtrlAtOrBefore (device, deviceCtrl, 0, fallbackDev);
    uint8_t mixv = mix != nullptr
                       ? lastCtrlAtOrBefore (*mix, (uint8_t) mixCtrl, 0, fallbackMix)
                       : fallbackMix;
    auto routed = [mix, mixMatch] (uint8_t m) -> bool
    {
        if (mix == nullptr)
            return true;
        return mixMatch < 0 ? m != 0 : m == (uint8_t) mixMatch;
    };
    auto onOf = [&]() -> int { return (dev != 0 && routed (mixv)) ? 1 : 0; };

    struct Hit { int tick; bool fromMix; uint8_t value; };
    std::vector<Hit> hits;
    for (const auto& e : device)
        if (e.ctrl == deviceCtrl)
            hits.push_back ({ e.tick, false, e.value });
    if (mix != nullptr)
        for (const auto& e : *mix)
            if (e.ctrl == (uint8_t) mixCtrl)
                hits.push_back ({ e.tick, true, e.value });
    std::sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b)
    {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return (int) a.fromMix < (int) b.fromMix;
    });

    int last = onOf();
    out.push_back ({ 0, order, "enabled", last });
    for (const auto& h : hits)
    {
        if (h.tick <= 0)
            continue;
        if (h.fromMix)
            mixv = h.value;
        else
            dev = h.value;
        const int v = onOf();
        if (v == last)
            continue;
        last = v;
        out.push_back ({ h.tick, order, "enabled", v });
    }
}

juce::String formatFxPrintBlock (std::vector<FxPrintLine> lines, uint32_t alwaysMask)
{
    std::array<bool, kFxParamCap> moving {};
    for (const auto& l : lines)
    {
        if (l.order < 0 || l.order >= kFxParamCap)
            continue;
        if (l.tick > 0 || (alwaysMask & (1u << l.order)) != 0)
            moving[(size_t) l.order] = true;
    }

    bool any = false;
    for (bool m : moving)
        any = any || m;
    if (! any)
        return {};

    std::array<FxPrintLine, kFxParamCap> start {};
    std::array<bool, kFxParamCap> haveStart {};
    std::vector<FxPrintLine> kept;
    for (const auto& l : lines)
    {
        if (l.order < 0 || l.order >= kFxParamCap || ! moving[(size_t) l.order] || l.name == nullptr)
            continue;
        if (l.tick <= 0)
        {
            start[(size_t) l.order] = l;
            haveStart[(size_t) l.order] = true;
        }
        else
        {
            kept.push_back (l);
        }
    }
    for (int i = 0; i < kFxParamCap; ++i)
        if (haveStart[(size_t) i])
            kept.push_back (start[(size_t) i]);

    std::sort (kept.begin(), kept.end(), [] (const FxPrintLine& a, const FxPrintLine& b)
    {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.order < b.order;
    });

    juce::String out;
    out << "\nopenrb-print 1\nppq: " << kFxPrintPpq << "\n";
    for (const auto& l : kept)
        out << l.tick << " " << l.name << " " << l.value << "\n";
    return out;
}
} // namespace

const char* ModloaderProcessor::auxName (int index)
{
    if (index < 0 || index >= kAuxBuses)
        return "";
    return kAuxNames[index];
}

const juce::String ModloaderProcessor::getOutputChannelName (int index) const
{
    int busIdx = 0;
    const int ch = getOffsetInBusBufferForAbsoluteChannelIndex (false, index, busIdx);
    if (ch < 0)
        return {};

    auto* bus = getBus (false, busIdx);
    if (bus == nullptr)
        return {};

    auto name = bus->getName();
    const auto layout = bus->getCurrentLayout();
    if (layout.size() > 1)
        name += " " + juce::AudioChannelSet::getAbbreviatedChannelTypeName (layout.getTypeOfChannel (ch));
    return name;
}

juce::String ModloaderProcessor::p808Id (int ctrl)
{
    return "808_" + juce::String (ctrl);
}

juce::String ModloaderProcessor::p909Id (int ctrl)
{
    return "909_" + juce::String (ctrl);
}

juce::String ModloaderProcessor::directId (int aux)
{
    return "direct_" + juce::String (aux);
}

juce::String ModloaderProcessor::mixDirectId (int machine)
{
    return machine == 0 ? "mute_main_808" : "mute_main_909";
}

juce::AudioProcessor::BusesProperties ModloaderProcessor::createBuses()
{
    auto buses = BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
    for (int i = 0; i < kMachineBuses; ++i)
        buses = buses.withOutput (kMixNames[i], juce::AudioChannelSet::stereo(), true);
    for (int i = 0; i < kAuxBuses; ++i)
        buses = buses.withOutput (kAuxNames[i], juce::AudioChannelSet::mono(), false);
    return buses;
}

namespace
{
juce::File prefsDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("OpenRB");
}

juce::String loadRememberedPath (const juce::String& filename)
{
    const auto f = prefsDir().getChildFile (filename);
    if (! f.existsAsFile())
        return {};
    return f.loadFileAsString().trim();
}

void saveRememberedPath (const juce::String& filename, const juce::String& path)
{
    if (path.isEmpty())
        return;
    auto f = prefsDir().getChildFile (filename);
    f.getParentDirectory().createDirectory();
    f.replaceWithText (path);
}

constexpr auto kKeyPathFile = "Modloader.keypath";
constexpr auto kModsFolderFile = "Modloader.modsfolder";
}

ModloaderProcessor::ModloaderProcessor()
    : AudioProcessor (createBuses()),
      apvts (*this, nullptr, "PARAMS", createLayout()),
      engine_ (orb_modloader_new())
{
    muteMain808 = apvts.getRawParameterValue (mixDirectId (0));
    muteMain909 = apvts.getRawParameterValue (mixDirectId (1));

    if (engine_ == nullptr)
        return;

    const auto rememberedDat = loadRememberedPath (kKeyPathFile);
    if (rememberedDat.isNotEmpty())
        orb_modloader_set_dat_path (engine_.get(), rememberedDat.toRawUTF8());

    // ORB_MOD_DIR already applied in orb_modloader_new(); don't override it.
    if (juce::SystemStats::getEnvironmentVariable ("ORB_MOD_DIR", {}).isEmpty())
    {
        const auto rememberedMods = loadRememberedPath (kModsFolderFile);
        if (rememberedMods.isNotEmpty())
            orb_modloader_scan (engine_.get(), rememberedMods.toRawUTF8());
    }

    if (wrapperType == wrapperType_Standalone)
        orb_modloader_play (engine_.get());
}

void ModloaderProcessor::setKeyPath (const juce::String& path)
{
    if (engine_ != nullptr && path.isNotEmpty())
        orb_modloader_set_dat_path (engine_.get(), path.toRawUTF8());
    saveRememberedPath (kKeyPathFile, path);
}

void ModloaderProcessor::setModsFolder (const juce::String& path)
{
    if (engine_ != nullptr && path.isNotEmpty())
        orb_modloader_scan (engine_.get(), path.toRawUTF8());
    saveRememberedPath (kModsFolderFile, path);
}

void ModloaderProcessor::resetDevice()
{
    disarmPrint();
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            ranged->setValueNotifyingHost (ranged->getDefaultValue());

    std::array<uint8_t, kPatternBytes> empty {};
    for (int p = 0; p < 32; ++p)
        empty[(size_t) (p * kPatternLen + 1)] = 16;

    if (engine_)
    {
        orb_modloader_set_patterns (engine_.get(), 0, empty.data(), kPatternBytes);
        orb_modloader_set_patterns (engine_.get(), 1, empty.data(), kPatternBytes);
        orb_modloader_reset_song (engine_.get());
        orb_modloader_reset (engine_.get());
        if (wrapperType == wrapperType_Standalone)
            orb_modloader_play (engine_.get());
    }
    songFile = juce::File();
    songModName = {};
    songEffects_ = {};
}

bool ModloaderProcessor::machineHasEffects (int machine) const
{
    const bool is808 = machine == 0;
    const bool dist = (is808 ? songEffects_.dist_808 : songEffects_.dist_909) != 0;
    const uint8_t pcfId = is808 ? 3 : 4;
    const uint8_t pcfBit = is808 ? 1 : 2;
    const bool pcf = songEffects_.pcf_id == pcfId || (songEffects_.reserved[0] & pcfBit) != 0;
    const bool delay = (is808 ? songEffects_.delay_send_808 : songEffects_.delay_send_909) != 0;
    return dist || pcf || delay;
}

int ModloaderProcessor::effectStartBar (EffectPrintKind kind, int machine) const
{
    if (engine_ == nullptr)
        return 0;

    const bool is808 = machine == 0;
    const auto mix = copyTrackEvents (engine_.get(), 0);
    int tick = -1;
    bool snapshotStart = false;
    switch (kind)
    {
        case EffectPrintKind::Pcf:
        {
            const uint8_t id = is808 ? 3 : 4;
            tick = firstCtrlTick (mix, kMixPcfId, false, id);
            snapshotStart = songEffects_.pcf_id == id;
            break;
        }
        case EffectPrintKind::Distortion:
        {
            tick = firstCtrlTick (mix, is808 ? kMixDist808 : kMixDist909, true, 0);
            snapshotStart = (is808 ? songEffects_.dist_808 : songEffects_.dist_909) != 0;
            break;
        }
        case EffectPrintKind::Delay:
        {
            tick = firstCtrlTick (mix, is808 ? kMixDelaySend808 : kMixDelaySend909, true, 0);
            snapshotStart = (is808 ? songEffects_.delay_send_808 : songEffects_.delay_send_909) != 0;
            break;
        }
    }
    if (tick >= 0)
        return tickToStartBar (tick);
    return snapshotStart ? 1 : 0;
}

juce::String ModloaderProcessor::formatEffectPrint (EffectPrintKind kind, int machine) const
{
    if (engine_ == nullptr)
        return {};

    std::vector<FxPrintLine> lines;
    switch (kind)
    {
        case EffectPrintKind::Delay:
        {
            const auto mix = copyTrackEvents (engine_.get(), 0);
            const auto delay = copyTrackEvents (engine_.get(), 5);
            const uint8_t send = machine == 0 ? kMixDelaySend808 : kMixDelaySend909;
            appendEnableTimeline (lines, 0, delay, 0, nullptr, -1, -1, 1, 0);
            appendMappedCtrl (lines, mix, send, 1, "mix", nullptr);
            appendMappedCtrl (lines, delay, 1, 2, "steps", nullptr);
            appendMappedCtrl (lines, delay, 2, 3, "mode", nullptr);
            appendMappedCtrl (lines, delay, 3, 4, "feedback", nullptr);
            appendMappedCtrl (lines, delay, 4, 5, "pan", nullptr);
            break;
        }
        case EffectPrintKind::Distortion:
        {
            const auto mix = copyTrackEvents (engine_.get(), 0);
            const auto dist = copyTrackEvents (engine_.get(), 6);
            const uint8_t distCtrl = machine == 0 ? kMixDist808 : kMixDist909;
            appendEnableTimeline (lines, 0, dist, 0, &mix, distCtrl, -1, 1, 0);
            appendMappedCtrl (lines, dist, 1, 1, "amount", nullptr);
            appendMappedCtrl (lines, dist, 2, 2, "shape", nullptr);
            break;
        }
        case EffectPrintKind::Pcf:
        {
            const auto mix = copyTrackEvents (engine_.get(), 0);
            const auto pcf = copyTrackEvents (engine_.get(), 7);
            const uint8_t id = machine == 0 ? 3 : 4;
            appendEnableTimeline (lines, 0, pcf, 0, &mix, kMixPcfId, (int) id, 1, 0);
            appendMappedCtrl (lines, pcf, 1, 1, "freq", nullptr);
            appendMappedCtrl (lines, pcf, 2, 2, "res", nullptr);
            appendMappedCtrl (lines, pcf, 3, 3, "amount", nullptr);
            appendMappedCtrl (lines, pcf, 4, 4, "pattern", nullptr);
            appendMappedCtrl (lines, pcf, 5, 5, "decay", nullptr);
            appendMappedCtrl (lines, pcf, 6, 6, "mode", pcfModeToPlugin);
            break;
        }
    }
    return formatFxPrintBlock (std::move (lines), 1u);
}

int ModloaderProcessor::effectSnapshotEnable (EffectPrintKind kind, int machine) const
{
    if (engine_ == nullptr)
        return 0;
    const bool is808 = machine == 0;
    switch (kind)
    {
        case EffectPrintKind::Delay:
        {
            const auto delay = copyTrackEvents (engine_.get(), 5);
            return lastCtrlAtOrBefore (delay, 0, 0, 1) != 0 ? 1 : 0;
        }
        case EffectPrintKind::Distortion:
        {
            const auto mix = copyTrackEvents (engine_.get(), 0);
            const auto dist = copyTrackEvents (engine_.get(), 6);
            const uint8_t distCtrl = is808 ? kMixDist808 : kMixDist909;
            const uint8_t dev = lastCtrlAtOrBefore (dist, 0, 0, 1);
            const uint8_t routed = lastCtrlAtOrBefore (mix, distCtrl, 0, 0);
            return (dev != 0 && routed != 0) ? 1 : 0;
        }
        case EffectPrintKind::Pcf:
        {
            const auto mix = copyTrackEvents (engine_.get(), 0);
            const auto pcf = copyTrackEvents (engine_.get(), 7);
            const uint8_t id = is808 ? 3 : 4;
            const uint8_t dev = lastCtrlAtOrBefore (pcf, 0, 0, 1);
            const uint8_t pcfId = lastCtrlAtOrBefore (mix, kMixPcfId, 0, 0);
            return (dev != 0 && pcfId == id) ? 1 : 0;
        }
    }
    return 0;
}

juce::String ModloaderProcessor::songDisplayName() const
{
    if (! songFile.getFullPathName().isNotEmpty())
        return "unsaved";
    const auto name = songFile.getFileNameWithoutExtension();
    return name.isNotEmpty() ? name : "unsaved";
}

void ModloaderProcessor::applyLivePanelParams()
{
    if (engine_ == nullptr)
        return;
    OrbModloaderParams live {};
    orb_modloader_live_params (engine_.get(), &live);
    OrbModloaderSong song {};
    std::memcpy (song.p808, live.p808, sizeof song.p808);
    std::memcpy (song.p909, live.p909, sizeof song.p909);
    // Song mode writes seq.shuffle from this knob every block. Copying the engine
    // value back here restored the default 64 after a song load.
    song.shuffle = (uint8_t) juce::jlimit (0, 127, asInt ("shuffle", 64));
    song.mix808 = live.mix808;
    song.mix909 = live.mix909;
    song.mix808_pan = live.mix808_pan;
    song.mix909_pan = live.mix909_pan;
    song.dist_808 = live.dist808;
    song.dist_909 = live.dist909;
    song.pcf_id = live.pcf_id;
    applySongParams (song);
}

void ModloaderProcessor::setMachineDist (int machine, bool on)
{
    const auto id = machine == 0 ? "dist_808" : "dist_909";
    if (auto* p = apvts.getParameter (id))
        p->setValueNotifyingHost (on ? 1.0f : 0.0f);
    if (! on)
        return;
    if (machine == 0)
        songEffects_.dist_808 = 1;
    else
        songEffects_.dist_909 = 1;
    songEffects_.dist_enabled = 1;
}

void ModloaderProcessor::setMachinePcf (int machine, bool on)
{
    auto* p808 = apvts.getParameter ("pcf_808");
    auto* p909 = apvts.getParameter ("pcf_909");
    if (on)
    {
        if (machine == 0)
        {
            if (p808 != nullptr)
                p808->setValueNotifyingHost (1.0f);
            if (p909 != nullptr)
                p909->setValueNotifyingHost (0.0f);
            songEffects_.pcf_id = 3;
            songEffects_.reserved[0] |= 1;
        }
        else
        {
            if (p909 != nullptr)
                p909->setValueNotifyingHost (1.0f);
            if (p808 != nullptr)
                p808->setValueNotifyingHost (0.0f);
            songEffects_.pcf_id = 4;
            songEffects_.reserved[0] |= 2;
        }
        songEffects_.pcf_enabled = 1;
        return;
    }
    if (machine == 0)
    {
        if (p808 != nullptr)
            p808->setValueNotifyingHost (0.0f);
        if (songEffects_.pcf_id == 3)
            songEffects_.pcf_id = 0;
        return;
    }
    if (p909 != nullptr)
        p909->setValueNotifyingHost (0.0f);
    if (songEffects_.pcf_id == 4)
        songEffects_.pcf_id = 0;
}

void ModloaderProcessor::applySongParams (const OrbModloaderSong& song)
{
    auto setParam = [this] (const juce::String& id, int value, bool isBool)
    {
        auto* p = apvts.getParameter (id);
        if (p == nullptr)
            return;
        if (isBool)
            p->setValueNotifyingHost (value != 0 ? 1.0f : 0.0f);
        else
            p->setValueNotifyingHost (p->convertTo0to1 ((float) value));
    };

    setParam (p808Id (0), song.p808[0], true);
    setParam (p808Id (1), song.p808[1], false);
    for (int i = 2; i < ORB_MODLOADER_P808; ++i)
        setParam (p808Id (i), song.p808[i], isSwitch808 (i));

    setParam (p909Id (0), song.p909[0], true);
    setParam (p909Id (1), song.p909[1], false);
    for (int i = 2; i < ORB_MODLOADER_P909; ++i)
        setParam (p909Id (i), song.p909[i], false);

    setParam ("shuffle", song.shuffle, false);
    setParam ("mix_808", song.mix808, false);
    setParam ("mix_909", song.mix909, false);
    setParam ("pan_808", song.mix808_pan, false);
    setParam ("pan_909", song.mix909_pan, false);
    setParam ("dist_808", song.dist_808, true);
    setParam ("dist_909", song.dist_909, true);
    setParam ("pcf_808", song.pcf_id == 3, true);
    setParam ("pcf_909", song.pcf_id == 4, true);
}

void ModloaderProcessor::fillSongParams (OrbModloaderSong& song) const
{
    std::memset (&song, 0, sizeof song);
    song.p808[0] = asInt (p808Id (0), kDefault808[0]) != 0 ? 1 : 0;
    song.p808[1] = (uint8_t) juce::jlimit (0, 31, asInt (p808Id (1), kDefault808[1]));
    for (int i = 2; i < ORB_MODLOADER_P808; ++i)
    {
        const int v = asInt (p808Id (i), kDefault808[i]);
        song.p808[i] = isSwitch808 (i) ? (v != 0 ? 1 : 0) : (uint8_t) juce::jlimit (0, 127, v);
    }
    song.p909[0] = asInt (p909Id (0), kDefault909[0]) != 0 ? 1 : 0;
    song.p909[1] = (uint8_t) juce::jlimit (0, 31, asInt (p909Id (1), kDefault909[1]));
    for (int i = 2; i < ORB_MODLOADER_P909; ++i)
        song.p909[i] = (uint8_t) juce::jlimit (0, 127, asInt (p909Id (i), kDefault909[i]));
    song.shuffle = (uint8_t) juce::jlimit (0, 127, asInt ("shuffle", 64));
    song.mix808 = (uint8_t) juce::jlimit (0, 127, asInt ("mix_808", 100));
    song.mix909 = (uint8_t) juce::jlimit (0, 127, asInt ("mix_909", 100));
    song.mix808_pan = (uint8_t) juce::jlimit (0, 127, asInt ("pan_808", 64));
    song.mix909_pan = (uint8_t) juce::jlimit (0, 127, asInt ("pan_909", 64));
    song.dist_808 = asInt ("dist_808", 0) != 0 ? 1 : 0;
    song.dist_909 = asInt ("dist_909", 0) != 0 ? 1 : 0;
    song.pcf_id = asInt ("pcf_909", 0) != 0 ? 4 : (asInt ("pcf_808", 0) != 0 ? 3 : 0);

    if (engine_ == nullptr)
        return;
    char name[ORB_MODLOADER_MOD_NAME] {};
    orb_modloader_mod_name (engine_.get(), orb_modloader_current_mod (engine_.get()),
                            name, (int) sizeof name);
    std::strncpy (song.mod_name, name, sizeof song.mod_name - 1);
    copySongFx (song, songEffects_);
    song.dist_808 = asInt ("dist_808", 0) != 0 ? 1 : 0;
    song.dist_909 = asInt ("dist_909", 0) != 0 ? 1 : 0;
    song.pcf_id = asInt ("pcf_909", 0) != 0 ? 4 : (asInt ("pcf_808", 0) != 0 ? 3 : 0);
    if (song.dist_808 != 0 || song.dist_909 != 0)
        song.dist_enabled = 1;
    if (song.pcf_id != 0)
        song.pcf_enabled = 1;
    if (song.pcf_id == 3)
        song.reserved[0] |= 1;
    if (song.pcf_id == 4)
        song.reserved[0] |= 2;

    OrbModloaderTransport t {};
    orb_modloader_transport (engine_.get(), &t);
    song.song_mode = t.song_mode != 0 ? 1 : 0;
    song.loop_on = t.loop_on != 0 ? 1 : 0;
    song.tempo = orb_modloader_tempo_milli (engine_.get());
    const auto startBar = juce::jmax (1, t.start);
    const auto lengthBars = juce::jmax (1, t.length);
    song.loop_start = (uint32_t) (startBar - 1) * kTicksPerBar;
    song.loop_end = song.loop_start + (uint32_t) lengthBars * kTicksPerBar;
}

bool ModloaderProcessor::loadSongFile (const juce::File& file, juce::String& error)
{
    error.clear();
    if (engine_ == nullptr)
    {
        error = "engine is not ready";
        return false;
    }
    if (! file.existsAsFile())
    {
        error = "file not found";
        return false;
    }
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() == 0)
    {
        error = "could not read song";
        return false;
    }

    OrbModloaderSong song {};
    if (orb_modloader_load_song (engine_.get(), (const uint8_t*) data.getData(),
                                 (int) data.getSize(), &song) == 0)
    {
        char buf[256] {};
        orb_modloader_status (engine_.get(), buf, (int) sizeof buf);
        error = juce::String::fromUTF8 (buf);
        if (error.isEmpty())
            error = "not a ReBirth 2.x song";
        return false;
    }

    disarmPrint();
    applySongParams (song);
    songEffects_ = {};
    copySongFx (songEffects_, song);
    applyLivePanelParams();
    if (auto* p = apvts.getParameter ("shuffle"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) song.shuffle));
    songModName = juce::String::fromUTF8 (song.mod_name).trim();
    songFile = file;
    return true;
}

bool ModloaderProcessor::saveSongFile (const juce::File& file, juce::String& error)
{
    error.clear();
    if (engine_ == nullptr)
    {
        error = "engine is not ready";
        return false;
    }

    OrbModloaderSong song {};
    fillSongParams (song);
    const int needed = orb_modloader_save_song (engine_.get(), &song, nullptr, 0);
    if (needed <= 0)
    {
        char buf[256] {};
        orb_modloader_status (engine_.get(), buf, (int) sizeof buf);
        error = juce::String::fromUTF8 (buf);
        if (error.isEmpty())
            error = "could not write song";
        return false;
    }

    juce::HeapBlock<uint8_t> buf ((size_t) needed);
    const int n = orb_modloader_save_song (engine_.get(), &song, buf.getData(), needed);
    if (n <= 0 || n > needed)
    {
        char msg[256] {};
        orb_modloader_status (engine_.get(), msg, (int) sizeof msg);
        error = juce::String::fromUTF8 (msg);
        if (error.isEmpty())
            error = "could not write song";
        return false;
    }

    if (! file.replaceWithData (buf.getData(), (size_t) n))
    {
        error = "could not save " + file.getFileName();
        return false;
    }
    songFile = file;
    return true;
}

int ModloaderProcessor::loadModByName (const juce::String& name)
{
    if (engine_ == nullptr)
        return -1;
    const int n = orb_modloader_mod_count (engine_.get());
    const auto wanted = name.trim();
    int caseInsensitive = -1;
    char buf[256] {};
    for (int i = 0; i < n; ++i)
    {
        orb_modloader_mod_name (engine_.get(), i, buf, (int) sizeof buf);
        const auto have = juce::String::fromUTF8 (buf).trim();
        if (have == wanted)
        {
            orb_modloader_load (engine_.get(), i);
            return i;
        }
        if (caseInsensitive < 0 && have.equalsIgnoreCase (wanted))
            caseInsensitive = i;
    }
    if (caseInsensitive >= 0)
    {
        orb_modloader_load (engine_.get(), caseInsensitive);
        return caseInsensitive;
    }
    return -1;
}

ModloaderProcessor::~ModloaderProcessor()
{
    cancelPendingUpdate();
    for (int i = 0; i < kPrintParams; ++i)
        endPrintGesture (i);
    if (engine_)
        orb_modloader_set_print (engine_.get(), 0);
}

int ModloaderProcessor::printIndex (uint8_t machine, uint8_t ctrl)
{
    if (machine == 0 && ctrl < ORB_MODLOADER_P808)
        return (int) ctrl;
    if (machine == 1 && ctrl < ORB_MODLOADER_P909)
        return ORB_MODLOADER_P808 + (int) ctrl;
    if (machine == 2 && ctrl == 0)
        return kPrintMix808;
    if (machine == 2 && ctrl == 1)
        return kPrintMix909;
    if (machine == 2 && ctrl == 2)
        return kPrintMix808Pan;
    if (machine == 2 && ctrl == 3)
        return kPrintMix909Pan;
    return -1;
}

bool ModloaderProcessor::printIsBool (int index)
{
    if (index == 0 || index == ORB_MODLOADER_P808)
        return true;
    return index < ORB_MODLOADER_P808 && isSwitch808 (index);
}

bool ModloaderProcessor::printIsStepped (int index)
{
    if (printIsBool (index))
        return true;
    return index == 1 || index == ORB_MODLOADER_P808 + 1;
}

bool ModloaderProcessor::printLatchesAtBar (int index)
{
    return index == 0 || index == 1
        || index == ORB_MODLOADER_P808 || index == ORB_MODLOADER_P808 + 1;
}

int ModloaderProcessor::printFireTick (int index, int tick) const
{
    if (tick <= 0)
        return tick;
    if (printLatchesAtBar (index))
        return tick - (int) (kTicksPerBar / 2);
    // 808 sample selectors are read at the 16th-note hit. A TRAK event on that step
    // is written a 32nd early so a late DAW stamp still precedes the trigger. Events
    // on the odd 32nd stay put: those are meant for the next 16th.
    if (index < ORB_MODLOADER_P808 && isSwitch808 (index)
        && tick % kTicksPerSixteenth == 0)
        return tick - kTicksPerPos;
    // Step-recorded knobs jump on the bar. Write them a 32nd early (on the TRAK
    // grid) so a late DAW stamp still precedes the downbeat. Larger or off-grid
    // leads snap in Reason and land after the bar again.
    if (index >= 0 && index < kPrintParams && printBarJumps[(size_t) index]
        && tick % (int) kTicksPerBar == 0)
        return tick - kTicksPerPos;
    return tick;
}

bool ModloaderProcessor::printAlwaysLane (int index)
{
    return printLatchesAtBar (index) || index >= kPrintMix808;
}

bool ModloaderProcessor::printLaneParts (int index, uint8_t& machine, uint8_t& ctrl)
{
    if (index < 0 || index >= kPrintParams)
        return false;
    machine = 0;
    ctrl = 0;
    if (index < ORB_MODLOADER_P808)
        ctrl = (uint8_t) index;
    else if (index < kPrintDrums)
    {
        machine = 1;
        ctrl = (uint8_t) (index - ORB_MODLOADER_P808);
    }
    else
    {
        machine = 2;
        if (index == kPrintMix808)
            ctrl = 0;
        else if (index == kPrintMix909)
            ctrl = 1;
        else if (index == kPrintMix808Pan)
            ctrl = 2;
        else
            ctrl = 3;
    }
    return true;
}

bool ModloaderProcessor::printLaneIsBarJumps (const std::vector<OrbModloaderTrackEvent>& events,
                                              int index)
{
    // ReBirth step-record (stopped, bar-step) stores knobs on bar lines. Those
    // should print as Hold jumps, not as a ramp across the previous bar.
    if (printIsStepped (index))
        return false;
    uint8_t machine = 0;
    uint8_t ctrl = 0;
    if (! printLaneParts (index, machine, ctrl))
        return false;
    bool any = false;
    for (const auto& e : events)
    {
        if (e.machine != machine || e.ctrl != ctrl || e.tick <= 0)
            continue;
        if (e.tick % (int) kTicksPerBar != 0)
            return false;
        any = true;
    }
    return any;
}

bool ModloaderProcessor::printWritesJump (int index) const
{
    if (printIsStepped (index))
        return true;
    if (index < 0 || index >= kPrintParams)
        return false;
    return printBarJumps[(size_t) index];
}

juce::RangedAudioParameter* ModloaderProcessor::printParam (int index) const
{
    juce::String id;
    if (index < ORB_MODLOADER_P808)
        id = p808Id (index);
    else if (index < kPrintDrums)
        id = p909Id (index - ORB_MODLOADER_P808);
    else if (index == kPrintMix808)
        id = "mix_808";
    else if (index == kPrintMix909)
        id = "mix_909";
    else if (index == kPrintMix808Pan)
        id = "pan_808";
    else if (index == kPrintMix909Pan)
        id = "pan_909";
    else
        return nullptr;
    return apvts.getParameter (id);
}

void ModloaderProcessor::queuePrintValue (int index, uint8_t value)
{
    if (index < 0 || index >= kPrintParams)
        return;
    printPending[(size_t) index] = value;
    printPendingOn[(size_t) index] = true;
}

bool ModloaderProcessor::printValueAt (const std::vector<OrbModloaderTrackEvent>& events,
                                       int index, int tick, uint8_t& value)
{
    uint8_t machine = 0;
    uint8_t ctrl = 0;
    if (! printLaneParts (index, machine, ctrl))
        return false;
    bool found = false;
    for (const auto& e : events)
    {
        if (e.machine != machine || e.ctrl != ctrl || e.tick > tick)
            continue;
        found = true;
        value = e.value;
    }
    return found;
}

void ModloaderProcessor::setPrintParam (int index, uint8_t value)
{
    auto* p = printParam (index);
    if (p == nullptr)
        return;
    if (printIsBool (index))
        p->setValueNotifyingHost (value != 0 ? 1.0f : 0.0f);
    else
        p->setValueNotifyingHost (p->convertTo0to1 ((float) value));
}

void ModloaderProcessor::beginPrintGesture (int index)
{
    if (index < 0 || index >= kPrintParams || printGesturing[(size_t) index])
        return;
    if (auto* p = printParam (index))
    {
        p->beginChangeGesture();
        printGesturing[(size_t) index] = true;
    }
}

void ModloaderProcessor::endPrintGesture (int index)
{
    if (index < 0 || index >= kPrintParams || ! printGesturing[(size_t) index])
        return;
    printGesturing[(size_t) index] = false;
    if (auto* p = printParam (index))
        p->endChangeGesture();
}

void ModloaderProcessor::writePrintParam (int index, uint8_t value)
{
    beginPrintGesture (index);
    setPrintParam (index, value);
    if (printWritesJump (index))
        endPrintGesture (index);
}

void ModloaderProcessor::chasePrintAt (int tick)
{
    for (int i = 0; i < kPrintParams; ++i)
    {
        if (! printMoving[(size_t) i])
            continue;
        uint8_t value = 0;
        if (printValueAt (printEvents, i, tick, value))
            queuePrintValue (i, value);
    }
}

void ModloaderProcessor::emitPrintRange (int fromTick, int toTick, bool latchesOnly)
{
    for (const auto& e : printEvents)
    {
        const int index = printIndex (e.machine, e.ctrl);
        if (index < 0 || ! printMoving[(size_t) index])
            continue;
        if (latchesOnly && ! printLatchesAtBar (index))
            continue;
        const int fireAt = printFireTick (index, e.tick);
        if (fireAt <= fromTick || fireAt > toTick)
            continue;
        queuePrintValue (index, e.value);
    }
}

void ModloaderProcessor::armPrint()
{
    if (wrapperType == wrapperType_Standalone || engine_ == nullptr)
        return;

    for (int i = 0; i < kPrintParams; ++i)
        endPrintGesture (i);

    const int n = orb_modloader_copy_song_events (engine_.get(), nullptr, 0);
    std::vector<OrbModloaderTrackEvent> events ((size_t) juce::jmax (0, n));
    if (n > 0)
        orb_modloader_copy_song_events (engine_.get(), events.data(), n);

    std::array<bool, kPrintParams> moving {};
    for (const auto& e : events)
    {
        const int index = printIndex (e.machine, e.ctrl);
        if (index < 0)
            continue;
        // Enable, Pattern, mixer Volume, and mixer Pan are the arrangement. Print
        // them even when they only exist at tick 0. Other knobs still need a later move.
        if (e.tick > 0 || printAlwaysLane (index))
            moving[(size_t) index] = true;
    }

    orb_modloader_set_record (engine_.get(), 0);
    orb_modloader_set_print (engine_.get(), 1);

    std::array<bool, kPrintParams> barJumps {};
    for (int i = 0; i < kPrintParams; ++i)
        if (moving[(size_t) i])
            barJumps[(size_t) i] = printLaneIsBarJumps (events, i);
    printBarJumps = barJumps;

    // Write tick-0 values while stopped so Latch/Write capture bar 1. Async updates
    // during the take are stamped at the message-thread playhead and can miss the start.
    // Enable/Pattern/switches and step-recorded knobs (bar-line only) are begin/set/end
    // jumps so hosts can record Hold; live-recorded knobs keep the gesture open to ramp.
    for (int i = 0; i < kPrintParams; ++i)
    {
        if (! moving[(size_t) i])
            continue;
        uint8_t value = 0;
        if (printValueAt (events, i, 0, value))
            writePrintParam (i, value);
        else if (! printWritesJump (i))
            beginPrintGesture (i);
    }

    {
        const juce::SpinLock::ScopedLockType sl (printLock);
        printEvents = std::move (events);
        printMoving = moving;
        printBarJumps = barJumps;
        printPendingOn.fill (false);
        printTakeActive = false;
        printLastTick = 0;
        printStopPending = false;
        printArmed = true;
    }
}

void ModloaderProcessor::disarmPrint()
{
    {
        const juce::SpinLock::ScopedLockType sl (printLock);
        printArmed = false;
        printTakeActive = false;
        printStopPending = false;
        printEvents.clear();
        printMoving.fill (false);
        printBarJumps.fill (false);
        printPendingOn.fill (false);
    }

    if (engine_)
        orb_modloader_set_print (engine_.get(), 0);

    for (int i = 0; i < kPrintParams; ++i)
        endPrintGesture (i);
}

bool ModloaderProcessor::setPrintArmed (bool on)
{
    if (on)
        armPrint();
    else
        disarmPrint();
    return isPrintArmed();
}

void ModloaderProcessor::runPrintTake (bool hostPlaying)
{
    if (wrapperType == wrapperType_Standalone || engine_ == nullptr || ! printArmed.load())
        return;

    OrbModloaderTransport t {};
    orb_modloader_transport (engine_.get(), &t);
    if (t.song_mode == 0)
        return;

    bool notify = false;
    {
        const juce::SpinLock::ScopedLockType sl (printLock);
        if (! printArmed.load())
            return;

        if (! hostPlaying)
        {
            if (printTakeActive)
            {
                printTakeActive = false;
                printArmed = false;
                printStopPending = true;
                notify = true;
            }
        }
        else
        {
            if (! printTakeActive)
            {
                // Bar 1 was written at arm (tick 0). Chasing the current tick, or
                // looking back a half bar for every lane, also queues 808 sample
                // switches whose fireAt is a 32nd early — last-write wins, and
                // Reason records the later sample as the song start.
                chasePrintAt (0);
                emitPrintRange (t.tick - (int) (kTicksPerBar / 2), t.tick, true);
                printTakeActive = true;
                printLastTick = 0;
            }
            else if (t.tick < printLastTick)
            {
                chasePrintAt (t.tick);
                emitPrintRange (t.tick - (int) (kTicksPerBar / 2), t.tick, true);
                printLastTick = t.tick;
            }
            else
            {
                emitPrintRange (printLastTick, t.tick);
                printLastTick = t.tick;
            }
            for (bool on : printPendingOn)
            {
                if (on)
                {
                    notify = true;
                    break;
                }
            }
        }
    }

    if (notify)
        triggerAsyncUpdate();
}

void ModloaderProcessor::handleAsyncUpdate()
{
    std::array<uint8_t, kPrintParams> values {};
    std::array<bool, kPrintParams> on {};
    bool stop = false;
    {
        const juce::SpinLock::ScopedLockType sl (printLock);
        values = printPending;
        on = printPendingOn;
        printPendingOn.fill (false);
        stop = printStopPending.exchange (false);
    }

    for (int i = 0; i < kPrintParams; ++i)
    {
        if (! on[(size_t) i])
            continue;
        writePrintParam (i, values[(size_t) i]);
    }

    if (stop)
    {
        {
            const juce::SpinLock::ScopedLockType sl (printLock);
            printEvents.clear();
            printMoving.fill (false);
            printBarJumps.fill (false);
            printPendingOn.fill (false);
            printTakeActive = false;
            printArmed = false;
        }
        if (engine_)
            orb_modloader_set_print (engine_.get(), 0);
        for (int i = 0; i < kPrintParams; ++i)
            endPrintGesture (i);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ModloaderProcessor::createLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterChoice;
    using juce::AudioParameterInt;
    using juce::ParameterID;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    const auto patterns = patternChoiceNames();
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { p808Id (0), 1 }, k808Names[0], kDefault808[0] != 0));
    p.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { p808Id (1), 1 }, k808Names[1], patterns, kDefault808[1]));
    for (int i = 2; i < ORB_MODLOADER_P808; ++i)
    {
        if (isSwitch808 (i))
            p.push_back (std::make_unique<AudioParameterBool> (
                ParameterID { p808Id (i), 1 }, k808Names[i], kDefault808[i] != 0));
        else
            p.push_back (std::make_unique<AudioParameterInt> (
                ParameterID { p808Id (i), 1 }, k808Names[i], 0, 127, kDefault808[i]));
    }

    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { p909Id (0), 1 }, k909Names[0], kDefault909[0] != 0));
    p.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { p909Id (1), 1 }, k909Names[1], patterns, kDefault909[1]));
    for (int i = 2; i < ORB_MODLOADER_P909; ++i)
        p.push_back (std::make_unique<AudioParameterInt> (
            ParameterID { p909Id (i), 1 }, k909Names[i], 0, 127, kDefault909[i]));

    // ReBirth default 64. Amounts 0–5 are identity (engine divides by 6).
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "shuffle", 1 }, "Shuffle", 0, 127, 64));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "mix_808", 1 }, "808 Volume", 0, 127, 100));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "mix_909", 1 }, "909 Volume", 0, 127, 100));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "pan_808", 1 }, "808 Pan", 0, 127, 64));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "pan_909", 1 }, "909 Pan", 0, 127, 64));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "dist_808", 1 }, "808 Distortion", false));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "dist_909", 1 }, "909 Distortion", false));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "pcf_808", 1 }, "808 PCF", false));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "pcf_909", 1 }, "909 PCF", false));

    for (int i = 0; i < kAuxBuses; ++i)
        p.push_back (std::make_unique<AudioParameterBool> (
            ParameterID { directId (i), 1 }, juce::String ("Mute ") + kAuxNames[i], false));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { mixDirectId (0), 1 }, "Mute 808", false));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { mixDirectId (1), 1 }, "Mute 909", false));

    return { p.begin(), p.end() };
}

void ModloaderProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostRate = sampleRate > 0.0 ? sampleRate : kEngineRate;
    exact44100 = ratesMatch (hostRate, kEngineRate);
    maxHostBlock = std::max (samplesPerBlock, 1);

    const int extra = 256 + (int) juce::WindowedSincInterpolator::getBaseLatency();
    const int maxEngine = (int) std::ceil ((double) maxHostBlock * kEngineRate / std::min (hostRate, kEngineRate))
                          + extra + kBlock;

    engineBuf.setSize (kTotalCh, maxEngine);
    engineFifo.setSize (kTotalCh, maxEngine + extra);
    scratch.setSize (1, std::max (maxHostBlock, maxEngine));
    engineBuf.clear();
    engineFifo.clear();
    scratch.clear();
    engineFifoFill = 0;

    for (auto& i : toHost)
        i.reset();

    if (engine_)
        orb_modloader_reset (engine_.get());
    if (wrapperType == wrapperType_Standalone && engine_)
        orb_modloader_play (engine_.get());

    clearPlayClock();
    setLatencySamples (latencySamples());
}

void ModloaderProcessor::releaseResources() {}

void ModloaderProcessor::reset()
{
    engineBuf.clear();
    engineFifo.clear();
    engineFifoFill = 0;
    for (auto& i : toHost)
        i.reset();
    if (engine_)
        orb_modloader_reset (engine_.get());
    clearPlayClock();
}

void ModloaderProcessor::clearPlayClock()
{
    havePlayOrigin = false;
    playOrigin = 0.0;
    haveLastRawPpq = false;
    lastRawPpq = 0.0;
    haveLastSamples = false;
    lastSamples = 0;
    if (engine_)
        orb_modloader_restart_clock (engine_.get());
}

int ModloaderProcessor::latencySamples() const
{
    const int delayLatency = (int) std::llround ((double) kBlock * hostRate / kEngineRate);
    if (exact44100)
        return delayLatency;

    const double base = juce::WindowedSincInterpolator::getBaseLatency();
    const double downHost = base * hostRate / kEngineRate;
    return delayLatency + (int) std::llround (downHost);
}

bool ModloaderProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (const auto& set : layouts.inputBuses)
        if (! set.isDisabled())
            return false;

    // Extra buses may be disabled (0), mono, or stereo. Rejecting stereo extras
    // makes hosts that apply one arrangement to every bus drop the Mix outs.
    for (int i = 1; i < layouts.outputBuses.size(); ++i)
        if (layouts.outputBuses.getUnchecked (i).size() > 2)
            return false;

    return true;
}

bool ModloaderProcessor::canApplyBusesLayout (const BusesLayout& layouts) const
{
    return isBusesLayoutSupported (layouts);
}

int ModloaderProcessor::asInt (const juce::String& id, int fallback) const
{
    if (auto* v = apvts.getRawParameterValue (id))
        return juce::roundToInt (v->load());
    return fallback;
}

uint8_t ModloaderProcessor::flagOf (const std::atomic<float>* v)
{
    return (v != nullptr && v->load() >= 0.5f) ? 1 : 0;
}

OrbModloaderParams ModloaderProcessor::readParams (bool bypass) const
{
    OrbModloaderParams params {};
    params.p808[0] = asInt (p808Id (0), kDefault808[0]) != 0 ? 1 : 0;
    params.p808[1] = (uint8_t) juce::jlimit (0, 31, asInt (p808Id (1), kDefault808[1]));
    for (int i = 2; i < ORB_MODLOADER_P808; ++i)
    {
        const int v = asInt (p808Id (i), kDefault808[i]);
        params.p808[i] = isSwitch808 (i) ? (v != 0 ? 1 : 0) : (uint8_t) juce::jlimit (0, 127, v);
    }
    params.p909[0] = asInt (p909Id (0), kDefault909[0]) != 0 ? 1 : 0;
    params.p909[1] = (uint8_t) juce::jlimit (0, 31, asInt (p909Id (1), kDefault909[1]));
    for (int i = 2; i < ORB_MODLOADER_P909; ++i)
        params.p909[i] = (uint8_t) juce::jlimit (0, 127, asInt (p909Id (i), kDefault909[i]));
    params.shuffle = (uint8_t) juce::jlimit (0, 127, asInt ("shuffle", 64));
    for (int i = 0; i < kAuxBuses; ++i)
        params.direct[i] = asInt (directId (i), 0) != 0 ? 1 : 0;
    params.bypass = bypass ? 1 : 0;
    params.mix808 = (uint8_t) juce::jlimit (0, 127, asInt ("mix_808", 100));
    params.mix909 = (uint8_t) juce::jlimit (0, 127, asInt ("mix_909", 100));
    params.direct_mix808 = flagOf (muteMain808);
    params.direct_mix909 = flagOf (muteMain909);
    params.mix808_pan = (uint8_t) juce::jlimit (0, 127, asInt ("pan_808", 64));
    params.mix909_pan = (uint8_t) juce::jlimit (0, 127, asInt ("pan_909", 64));
    params.dist808 = asInt ("dist_808", 0) != 0 ? 1 : 0;
    params.dist909 = asInt ("dist_909", 0) != 0 ? 1 : 0;
    params.pcf_id = asInt ("pcf_909", 0) != 0 ? 4 : (asInt ("pcf_808", 0) != 0 ? 3 : 0);
    return params;
}

static double startingQuarter (double rawPpq, bool hasPpq, juce::int64 samples, bool hasSamples, double bpm, double hostRate)
{
    const bool canDerive = hasSamples && hostRate > 0.0 && bpm > 0.0;
    const double derived = canDerive ? (double) samples * bpm / (hostRate * 60.0) : 0.0;
    if (!hasPpq)
        return canDerive ? derived : 0.0;
    if (canDerive && std::abs (rawPpq) < 1.0e-6 && derived > 0.25)
        return derived;
    return rawPpq;
}

OrbModloaderPlayhead ModloaderProcessor::readPlayhead (int nHost)
{
    OrbModloaderPlayhead head { 120.0, 0.0, 0, 0 };
    const bool standalone = wrapperType == wrapperType_Standalone;
    if (standalone)
    {
        const double bpm = engine_ != nullptr
                               ? (double) orb_modloader_tempo_milli (engine_.get()) / 1000.0
                               : 120.0;
        head.bpm = bpm > 0.0 ? bpm : 120.0;
        if (engine_ != nullptr)
        {
            OrbModloaderTransport t {};
            orb_modloader_transport (engine_.get(), &t);
            head.playing = t.playing;
        }
        return head;
    }

    head.host_driven = 1;

    if (auto* ph = getPlayHead())
    {
        if (auto info = ph->getPosition())
        {
            head.bpm = info->getBpm().orFallback (120.0);
            if (!(head.bpm > 0.0) || !std::isfinite (head.bpm))
                head.bpm = 120.0;

            const auto rawOpt = info->getPpqPosition();
            const bool hasPpq = rawOpt.hasValue() && std::isfinite (*rawOpt);
            const double rawPpq = hasPpq ? *rawOpt : 0.0;

            const auto sampleOpt = info->getTimeInSamples();
            const bool hasSamples = sampleOpt.hasValue();
            const juce::int64 samples = hasSamples ? *sampleOpt : 0;

            const double advance = (hostRate > 0.0 && nHost > 0)
                                       ? (double) nHost * head.bpm / (hostRate * 60.0)
                                       : 0.0;
            const double deltaPpq = (hasPpq && haveLastRawPpq) ? rawPpq - lastRawPpq : 0.0;
            const juce::int64 deltaSamples = (hasSamples && haveLastSamples) ? samples - lastSamples : 0;
            const bool transportPlaying = info->getIsPlaying() || info->getIsRecording();
            // Hosts that omit isPlaying still roll the playhead by about one buffer.
            // A locate or rewind is a jump, not playback — treating those as playing
            // fired step 0 whenever the DAW returned to the start.
            const bool ppqRolling = hasPpq && haveLastRawPpq && deltaPpq > 1.0e-9
                                    && std::abs (deltaPpq - advance) < std::max (1.0e-4, advance * 0.5);
            const bool samplesRolling = hasSamples && haveLastSamples && deltaSamples > 0
                                        && std::abs ((double) deltaSamples - (double) nHost)
                                               < std::max (1.0, (double) nHost * 0.5);
            head.playing = (transportPlaying || ppqRolling || samplesRolling) ? 1 : 0;

            if (!havePlayOrigin)
            {
                playOrigin = startingQuarter (rawPpq, hasPpq, samples, hasSamples, head.bpm, hostRate);
                havePlayOrigin = true;
            }

            double reported = playOrigin;
            if (head.playing != 0 && advance > 0.0)
            {
                const bool haveDelta = hasPpq && haveLastRawPpq;
                const double delta = haveDelta ? rawPpq - lastRawPpq : 0.0;
                const bool agrees = haveDelta && std::abs (delta - advance) < std::max (1.0e-4, advance * 0.5);
                const bool jumped = haveDelta && std::abs (delta) > std::max (1.0, advance * 4.0);
                if (agrees || jumped)
                    reported = rawPpq;
                playOrigin = reported + advance;
            }
            else if (head.playing == 0 && hasPpq && haveLastRawPpq
                     && std::abs (rawPpq - lastRawPpq) > std::max (1.0, advance * 4.0))
            {
                playOrigin = rawPpq;
                reported = rawPpq;
            }

            head.ppq = std::isfinite (reported) ? reported : 0.0;

            if (hasPpq)
            {
                lastRawPpq = rawPpq;
                haveLastRawPpq = true;
            }
            if (hasSamples)
            {
                lastSamples = samples;
                haveLastSamples = true;
            }
        }
    }
    return head;
}

void ModloaderProcessor::shiftFifo (juce::AudioBuffer<float>& fifo, int& fill, int used)
{
    if (used <= 0)
        return;
    if (used >= fill)
    {
        fill = 0;
        return;
    }
    const int remain = fill - used;
    for (int ch = 0; ch < fifo.getNumChannels(); ++ch)
        std::memmove (fifo.getWritePointer (ch), fifo.getReadPointer (ch) + used, (size_t) remain * sizeof (float));
    fill = remain;
}

void ModloaderProcessor::engineToHost (float* const* hostOut, int nHost, int nEngine, int nCh)
{
    const int cap = engineFifo.getNumSamples();
    const int take = juce::jmin (nEngine, cap - engineFifoFill);
    if (take > 0)
    {
        for (int ch = 0; ch < nCh; ++ch)
            std::memcpy (engineFifo.getWritePointer (ch) + engineFifoFill, engineBuf.getReadPointer (ch),
                         (size_t) take * sizeof (float));
        engineFifoFill += take;
    }

    const double speed = kEngineRate / hostRate;
    int used = 0;
    for (int ch = 0; ch < nCh; ++ch)
        if (hostOut[ch] != nullptr)
            used = toHost[ch].process (speed, engineFifo.getReadPointer (ch), hostOut[ch], nHost, engineFifoFill, 0);

    shiftFifo (engineFifo, engineFifoFill, juce::jmin (used, engineFifoFill));
}

void ModloaderProcessor::processBuffer (juce::AudioBuffer<float>& buffer, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;

    const int nHost = buffer.getNumSamples();
    if (nHost <= 0 || engine_ == nullptr)
        return;

    buffer.clear();

    auto params = readParams (bypass);
    const auto head = readPlayhead (nHost);

    auto main = getBusBuffer (buffer, false, 0);
    float* mainL = main.getNumChannels() > 0 ? main.getWritePointer (0) : scratch.getWritePointer (0);
    float* mainR = main.getNumChannels() > 1 ? main.getWritePointer (1)
                                             : (main.getNumChannels() > 0 ? main.getWritePointer (0)
                                                                          : scratch.getWritePointer (0));

    auto mix808Bus = getBusBuffer (buffer, false, 1);
    auto mix909Bus = getBusBuffer (buffer, false, 2);
    float* mix808L = mix808Bus.getNumChannels() > 0 ? mix808Bus.getWritePointer (0) : nullptr;
    float* mix808R = mix808Bus.getNumChannels() > 1 ? mix808Bus.getWritePointer (1) : nullptr;
    float* mix909L = mix909Bus.getNumChannels() > 0 ? mix909Bus.getWritePointer (0) : nullptr;
    float* mix909R = mix909Bus.getNumChannels() > 1 ? mix909Bus.getWritePointer (1) : nullptr;

    float* auxPtr[kAuxBuses];
    float* hostOut[kTotalCh] {};
    hostOut[0] = mainL;
    hostOut[1] = mainR;

    auto attachAux = [&] (int i, float*& dest)
    {
        auto bus = getBusBuffer (buffer, false, i + kAuxBus0);
        dest = bus.getNumChannels() > 0 ? bus.getWritePointer (0) : scratch.getWritePointer (0);
    };

    if (exact44100)
    {
        for (int i = 0; i < kAuxBuses; ++i)
            attachAux (i, auxPtr[i]);
        orb_modloader_process (engine_.get(), mainL, mainR, mix808L, mix808R, mix909L, mix909R,
                               auxPtr, nHost, &params, &head);
        runPrintTake (head.playing != 0);
        applyMainMute (mainL, mainR, mix808L, mix808R, mix909L, mix909R,
                       params.direct_mix808 != 0, params.direct_mix909 != 0, nHost);
        for (int i = 0; i < kAuxBuses; ++i)
            fillBusFrom (getBusBuffer (buffer, false, i + kAuxBus0), auxPtr[i], nHost);
        return;
    }

    const int nEngine = juce::jlimit (
        0, engineBuf.getNumSamples(),
        (int) std::ceil ((double) nHost * kEngineRate / hostRate) + 4);

    float* engAux[kAuxBuses];
    hostOut[2] = mix808L;
    hostOut[3] = mix808R;
    hostOut[4] = mix909L;
    hostOut[5] = mix909R;
    for (int i = 0; i < kAuxBuses; ++i)
    {
        engAux[i] = engineBuf.getWritePointer (i + 2 + kMixChannels);
        auto bus = getBusBuffer (buffer, false, i + kAuxBus0);
        hostOut[i + 2 + kMixChannels] = bus.getNumChannels() > 0 ? bus.getWritePointer (0) : nullptr;
    }
    if (nEngine > 0)
    {
        float* eng808L = engineBuf.getWritePointer (2);
        float* eng808R = engineBuf.getWritePointer (3);
        float* eng909L = engineBuf.getWritePointer (4);
        float* eng909R = engineBuf.getWritePointer (5);
        orb_modloader_process (engine_.get(), engineBuf.getWritePointer (0), engineBuf.getWritePointer (1),
                               eng808L, eng808R, eng909L, eng909R, engAux, nEngine, &params, &head);
        runPrintTake (head.playing != 0);
        applyMainMute (engineBuf.getWritePointer (0), engineBuf.getWritePointer (1),
                       eng808L, eng808R, eng909L, eng909R,
                       params.direct_mix808 != 0, params.direct_mix909 != 0, nEngine);
    }

    engineToHost (hostOut, nHost, nEngine, kTotalCh);
    for (int i = 0; i < kAuxBuses; ++i)
        fillBusFrom (getBusBuffer (buffer, false, i + kAuxBus0), hostOut[i + 2 + kMixChannels], nHost);
}

void ModloaderProcessor::applyIncomingMidi (const juce::MidiBuffer& midi)
{
    if (engine_ == nullptr)
        return;
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        if (! msg.isNoteOn())
            continue;
        int machine = 0, col = 0;
        if (! patternmidi::noteToDrum (msg.getNoteNumber(), machine, col))
            continue;
        orb_modloader_trigger (engine_.get(), machine, (uint8_t) col, msg.getVelocity());
    }
}

void ModloaderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    applyIncomingMidi (midi);
    processBuffer (buffer, false);
}

void ModloaderProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    applyIncomingMidi (midi);
    processBuffer (buffer, true);
}

std::optional<juce::String> ModloaderProcessor::getNameForMidiNoteNumber (int note, int)
{
    return patternmidi::nameForNote (note);
}

juce::AudioProcessorEditor* ModloaderProcessor::createEditor()
{
    return new ModloaderEditor (*this);
}

void ModloaderProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr)
        return;

    if (engine_)
    {
        std::vector<uint8_t> pat (kPatternBytes);
        if (orb_modloader_get_patterns (engine_.get(), 0, pat.data(), (int) pat.size()) > 0)
            xml->setAttribute ("pat808", juce::Base64::toBase64 (pat.data(), pat.size()));
        if (orb_modloader_get_patterns (engine_.get(), 1, pat.data(), (int) pat.size()) > 0)
            xml->setAttribute ("pat909", juce::Base64::toBase64 (pat.data(), pat.size()));

        char buf[1024] {};
        orb_modloader_mods_folder (engine_.get(), buf, (int) sizeof buf);
        xml->setAttribute ("modsFolder", juce::String::fromUTF8 (buf));
        orb_modloader_dat_path (engine_.get(), buf, (int) sizeof buf);
        xml->setAttribute ("datPath", juce::String::fromUTF8 (buf));
        xml->setAttribute ("modIndex", orb_modloader_current_mod (engine_.get()));
        orb_modloader_mod_path (engine_.get(), orb_modloader_current_mod (engine_.get()), buf, (int) sizeof buf);
        xml->setAttribute ("modPath", juce::String::fromUTF8 (buf));
        xml->setAttribute ("songPath", songFile.getFullPathName());
        xml->setAttribute ("fxDistEnabled", (int) songEffects_.dist_enabled);
        xml->setAttribute ("fxDistAmount", (int) songEffects_.dist_amount);
        xml->setAttribute ("fxDistShape", (int) songEffects_.dist_shape);
        xml->setAttribute ("fxDist808", (int) songEffects_.dist_808);
        xml->setAttribute ("fxDist909", (int) songEffects_.dist_909);
        xml->setAttribute ("fxPcfEnabled", (int) songEffects_.pcf_enabled);
        xml->setAttribute ("fxPcfFrequency", (int) songEffects_.pcf_frequency);
        xml->setAttribute ("fxPcfResonance", (int) songEffects_.pcf_resonance);
        xml->setAttribute ("fxPcfAmount", (int) songEffects_.pcf_amount);
        xml->setAttribute ("fxPcfWave", (int) songEffects_.pcf_wave);
        xml->setAttribute ("fxPcfDecay", (int) songEffects_.pcf_decay);
        xml->setAttribute ("fxPcfMode", (int) songEffects_.pcf_mode);
        xml->setAttribute ("fxPcfId", (int) songEffects_.pcf_id);
        xml->setAttribute ("fxDelayEnabled", (int) songEffects_.delay_enabled);
        xml->setAttribute ("fxDelaySteps", (int) songEffects_.delay_steps);
        xml->setAttribute ("fxDelayMode", (int) songEffects_.delay_mode);
        xml->setAttribute ("fxDelayFeedback", (int) songEffects_.delay_feedback);
        xml->setAttribute ("fxDelayPan", (int) songEffects_.delay_pan);
        xml->setAttribute ("fxDelaySend808", (int) songEffects_.delay_send_808);
        xml->setAttribute ("fxDelaySend909", (int) songEffects_.delay_send_909);
        xml->setAttribute ("fxPcfUsedMask", (int) songEffects_.reserved[0]);

        OrbModloaderSong song {};
        fillSongParams (song);
        const int needed = orb_modloader_save_song (engine_.get(), &song, nullptr, 0);
        if (needed > 0)
        {
            juce::HeapBlock<uint8_t> rbs ((size_t) needed);
            const int n = orb_modloader_save_song (engine_.get(), &song, rbs.getData(), needed);
            if (n > 0 && n <= needed)
                xml->setAttribute ("songRbs", juce::Base64::toBase64 (rbs.getData(), (size_t) n));
        }
        OrbModloaderTransport t {};
        orb_modloader_transport (engine_.get(), &t);
        xml->setAttribute ("songMode", t.song_mode);
    }

    copyXmlToBinary (*xml, destData);
}

void ModloaderProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    if (engine_ == nullptr)
        return;

    auto decodePat = [this, &xml] (const char* attr, int machine)
    {
        const auto b64 = xml->getStringAttribute (attr);
        if (b64.isEmpty())
            return;
        juce::MemoryOutputStream out;
        if (juce::Base64::convertFromBase64 (out, b64))
            orb_modloader_set_patterns (engine_.get(), machine, (const uint8_t*) out.getData(), (int) out.getDataSize());
    };
    decodePat ("pat808", 0);
    decodePat ("pat909", 1);

    const auto folder = xml->getStringAttribute ("modsFolder");
    if (folder.isNotEmpty())
        setModsFolder (folder);

    const auto dat = xml->getStringAttribute ("datPath");
    if (dat.isNotEmpty())
        setKeyPath (dat);

    const auto modPath = xml->getStringAttribute ("modPath");
    const int index = xml->getIntAttribute ("modIndex", 0);
    if (modPath.isNotEmpty())
    {
        const int n = orb_modloader_mod_count (engine_.get());
        char buf[1024];
        bool loaded = false;
        for (int i = 0; i < n; ++i)
        {
            orb_modloader_mod_path (engine_.get(), i, buf, (int) sizeof buf);
            if (modPath == juce::String::fromUTF8 (buf))
            {
                orb_modloader_load (engine_.get(), i);
                loaded = true;
                break;
            }
        }
        if (!loaded)
            orb_modloader_load (engine_.get(), index);
    }
    else
    {
        orb_modloader_load (engine_.get(), index);
    }

    const auto savedSong = xml->getStringAttribute ("songPath");
    songFile = savedSong.isNotEmpty() ? juce::File (savedSong) : juce::File();

    songEffects_ = {};
    songEffects_.dist_enabled = fxByte (*xml, "fxDistEnabled");
    songEffects_.dist_amount = fxByte (*xml, "fxDistAmount");
    songEffects_.dist_shape = fxByte (*xml, "fxDistShape");
    songEffects_.dist_808 = fxByte (*xml, "fxDist808");
    songEffects_.dist_909 = fxByte (*xml, "fxDist909");
    songEffects_.pcf_enabled = fxByte (*xml, "fxPcfEnabled");
    songEffects_.pcf_frequency = fxByte (*xml, "fxPcfFrequency");
    songEffects_.pcf_resonance = fxByte (*xml, "fxPcfResonance");
    songEffects_.pcf_amount = fxByte (*xml, "fxPcfAmount");
    songEffects_.pcf_wave = fxByte (*xml, "fxPcfWave");
    songEffects_.pcf_decay = fxByte (*xml, "fxPcfDecay");
    songEffects_.pcf_mode = fxByte (*xml, "fxPcfMode");
    songEffects_.pcf_id = fxByte (*xml, "fxPcfId");
    songEffects_.delay_enabled = fxByte (*xml, "fxDelayEnabled");
    songEffects_.delay_steps = fxByte (*xml, "fxDelaySteps");
    songEffects_.delay_mode = fxByte (*xml, "fxDelayMode");
    songEffects_.delay_feedback = fxByte (*xml, "fxDelayFeedback");
    songEffects_.delay_pan = fxByte (*xml, "fxDelayPan");
    songEffects_.delay_send_808 = fxByte (*xml, "fxDelaySend808");
    songEffects_.delay_send_909 = fxByte (*xml, "fxDelaySend909");
    songEffects_.reserved[0] = fxByte (*xml, "fxPcfUsedMask");

    const auto rbsB64 = xml->getStringAttribute ("songRbs");
    if (rbsB64.isNotEmpty())
    {
        juce::MemoryOutputStream out;
        if (juce::Base64::convertFromBase64 (out, rbsB64) && out.getDataSize() > 0)
        {
            OrbModloaderSong loaded {};
            if (orb_modloader_load_song (engine_.get(), (const uint8_t*) out.getData(),
                                         (int) out.getDataSize(), &loaded) != 0)
            {
                songEffects_ = {};
                copySongFx (songEffects_, loaded);
                songModName = juce::String::fromUTF8 (loaded.mod_name).trim();
                if (auto* p = apvts.getParameter ("shuffle"))
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) loaded.shuffle));
            }
        }
    }
    else if (xml->getIntAttribute ("songMode", 0) != 0)
        orb_modloader_set_song_mode (engine_.get(), 1);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ModloaderProcessor();
}
