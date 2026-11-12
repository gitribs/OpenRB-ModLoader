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
static_assert (sizeof (OrbModloaderParams) == 87);
static_assert (offsetof (OrbModloaderSong, dist_enabled) == 127);
static_assert (offsetof (OrbModloaderSong, delay_send_909) == 146);
static_assert (offsetof (OrbModloaderSong, song_mode) == 147);
static_assert (offsetof (OrbModloaderSong, tempo) == 152);
static_assert (sizeof (OrbModloaderSong) == 164);
static_assert (sizeof (OrbModloaderPlayhead) == 24);

constexpr uint32_t kTicksPerBar = 768;

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

void applyMainMute (float* mainL, float* mainR, const float* mix808, const float* mix909,
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
    const float* src = mute909 ? mix808 : (mute808 ? mix909 : nullptr);
    if (src == nullptr)
        return;
    copyMono (mainL, src, n);
    copyMono (mainR, src, n);
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
}

uint8_t fxByte (const juce::XmlElement& xml, const char* name)
{
    return (uint8_t) juce::jlimit (0, 255, xml.getIntAttribute (name, 0));
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
    const bool dist = songEffects_.dist_enabled != 0
                      && (is808 ? songEffects_.dist_808 : songEffects_.dist_909) != 0;
    const bool pcf = songEffects_.pcf_enabled != 0
                     && songEffects_.pcf_id == (is808 ? 3 : 4);
    const bool delay = songEffects_.delay_enabled != 0
                       && (is808 ? songEffects_.delay_send_808 : songEffects_.delay_send_909) != 0;
    return dist || pcf || delay;
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
    song.shuffle = live.shuffle;
    song.mix808 = live.mix808;
    song.mix909 = live.mix909;
    applySongParams (song);
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

    if (engine_ == nullptr)
        return;
    char name[ORB_MODLOADER_MOD_NAME] {};
    orb_modloader_mod_name (engine_.get(), orb_modloader_current_mod (engine_.get()),
                            name, (int) sizeof name);
    std::strncpy (song.mod_name, name, sizeof song.mod_name - 1);
    copySongFx (song, songEffects_);

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

    applySongParams (song);
    songEffects_ = {};
    copySongFx (songEffects_, song);
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

ModloaderProcessor::~ModloaderProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout ModloaderProcessor::createLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterChoice;
    using juce::AudioParameterInt;
    using juce::ParameterID;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { p808Id (0), 1 }, k808Names[0], kDefault808[0] != 0));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { p808Id (1), 1 }, k808Names[1], 0, 31, kDefault808[1]));
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
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { p909Id (1), 1 }, k909Names[1], 0, 31, kDefault909[1]));
    for (int i = 2; i < ORB_MODLOADER_P909; ++i)
        p.push_back (std::make_unique<AudioParameterInt> (
            ParameterID { p909Id (i), 1 }, k909Names[i], 0, 127, kDefault909[i]));

    // ReBirth default 64. Amounts 0–5 are identity (engine divides by 6).
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "shuffle", 1 }, "Shuffle", 0, 127, 64));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "mix_808", 1 }, "808 Volume", 0, 127, 100));
    p.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "mix_909", 1 }, "909 Volume", 0, 127, 100));

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

            const bool ppqMoving = hasPpq && haveLastRawPpq && std::abs (rawPpq - lastRawPpq) > 1.0e-9;
            const bool samplesMoving = hasSamples && haveLastSamples && samples != lastSamples;
            head.playing = (info->getIsPlaying() || info->getIsRecording() || ppqMoving || samplesMoving) ? 1 : 0;

            const double advance = (hostRate > 0.0 && nHost > 0)
                                       ? (double) nHost * head.bpm / (hostRate * 60.0)
                                       : 0.0;

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
        orb_modloader_process (engine_.get(), mainL, mainR, mix808L, mix909L, auxPtr, nHost, &params, &head);
        applyMainMute (mainL, mainR, mix808L, mix909L, params.direct_mix808 != 0, params.direct_mix909 != 0, nHost);
        fillBusFrom (getBusBuffer (buffer, false, 1), mix808L, nHost);
        fillBusFrom (getBusBuffer (buffer, false, 2), mix909L, nHost);
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
        float* eng808 = engineBuf.getWritePointer (2);
        float* eng909 = engineBuf.getWritePointer (4);
        orb_modloader_process (engine_.get(), engineBuf.getWritePointer (0), engineBuf.getWritePointer (1),
                               eng808, eng909, engAux, nEngine, &params, &head);
        std::memcpy (engineBuf.getWritePointer (3), eng808, (size_t) nEngine * sizeof (float));
        std::memcpy (engineBuf.getWritePointer (5), eng909, (size_t) nEngine * sizeof (float));
        applyMainMute (engineBuf.getWritePointer (0), engineBuf.getWritePointer (1), eng808, eng909,
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
