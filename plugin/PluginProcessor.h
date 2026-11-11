#pragma once

#include <JuceHeader.h>
#include "orb_modloader.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>

class ModloaderEditor;

class ModloaderProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kAuxBuses = ORB_MODLOADER_AUX;
    static constexpr int kMachineBuses = 2;
    static constexpr int kMixChannels = kMachineBuses * 2;
    static constexpr int kAuxBus0 = 1 + kMachineBuses;
    static constexpr int kPatternBytes = ORB_MODLOADER_PATTERN_BYTES;
    static constexpr int kPatternLen = kPatternBytes / 32;

    struct PatternClipboard
    {
        bool valid = false;
        int machine = -1;
        std::array<uint8_t, kPatternLen> bytes {};
    };

    ModloaderProcessor();
    ~ModloaderProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    bool canApplyBusesLayout (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    const juce::String getOutputChannelName (int index) const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    std::optional<juce::String> getNameForMidiNoteNumber (int note, int midiChannel) override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "OpenRB Modloader"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /// Remember a DAT or ISO path for this instance and later ones.
    void setKeyPath (const juce::String& path);
    /// Remember a mods folder for this instance and later ones.
    void setModsFolder (const juce::String& path);
    /// Restore default parameters and empty patterns, as a newly created instance.
    void resetDevice();

    /// Load a ReBirth 2.x `.rbs`: 808/909 knobs and all patterns. Returns false on error.
    bool loadSongFile (const juce::File& file, juce::String& error);
    /// Write a 4.2 `.rbs` with the current 808/909 knobs and patterns.
    bool saveSongFile (const juce::File& file, juce::String& error);
    /// Path of the last loaded or saved song; empty means unsaved.
    juce::File currentSong() const { return songFile; }
    /// Display name for the header: file stem, or "unsaved".
    juce::String songDisplayName() const;
    /// Mod named in the last loaded song (GLOB), empty if none.
    juce::String loadedSongModName() const { return songModName; }
    /// Distortion / PCF / Delay snapshot from the last loaded song (zeros if none).
    const OrbModloaderSong& songEffects() const { return songEffects_; }
    /// True when that machine had Distortion, PCF, or Delay routing in the loaded song.
    bool machineHasEffects (int machine) const;
    /// Load the scanned kit whose name matches `name` (case-insensitive). Returns the index, or -1.
    int loadModByName (const juce::String& name);

    juce::AudioProcessorValueTreeState apvts;
    PatternClipboard patternClip;
    OrbModloader* engine() noexcept { return engine_.get(); }

    static const char* auxName (int index);
    static juce::String p808Id (int ctrl);
    static juce::String p909Id (int ctrl);
    static juce::String directId (int aux);
    static juce::String mixDirectId (int machine);

private:
    struct EngineDeleter
    {
        void operator() (OrbModloader* p) const noexcept { orb_modloader_free (p); }
    };

    static juce::AudioProcessor::BusesProperties createBuses();
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void processBuffer (juce::AudioBuffer<float>& buffer, bool bypass);
    void applyIncomingMidi (const juce::MidiBuffer& midi);
    OrbModloaderParams readParams (bool bypass) const;
    OrbModloaderPlayhead readPlayhead (int nHost);
    void clearPlayClock();
    int latencySamples() const;
    void engineToHost (float* const* hostOut, int nHost, int nEngine, int nCh);
    void shiftFifo (juce::AudioBuffer<float>& fifo, int& fill, int used);
    int asInt (const juce::String& id, int fallback) const;
    static uint8_t flagOf (const std::atomic<float>* v);

    void applySongParams (const OrbModloaderSong& song);
    void fillSongParams (OrbModloaderSong& song) const;

    std::unique_ptr<OrbModloader, EngineDeleter> engine_;
    std::atomic<float>* muteMain808 = nullptr;
    std::atomic<float>* muteMain909 = nullptr;
    juce::File songFile;
    juce::String songModName;
    OrbModloaderSong songEffects_ {};
    double hostRate = 44100.0;
    bool exact44100 = true;
    int maxHostBlock = 0;

    static constexpr int kTotalCh = 2 + kMixChannels + kAuxBuses;
    juce::WindowedSincInterpolator toHost[kTotalCh];
    juce::AudioBuffer<float> engineBuf;
    juce::AudioBuffer<float> engineFifo;
    juce::AudioBuffer<float> scratch;
    int engineFifoFill = 0;

    bool havePlayOrigin = false;
    double playOrigin = 0.0;
    bool haveLastRawPpq = false;
    double lastRawPpq = 0.0;
    bool haveLastSamples = false;
    juce::int64 lastSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModloaderProcessor)
};
