#pragma once

#include <JuceHeader.h>
#include "orb_modloader.h"

#include <array>
#include <memory>

class ModloaderEditor;

class ModloaderProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kAuxBuses = ORB_MODLOADER_AUX;
    static constexpr int kPatternBytes = ORB_MODLOADER_PATTERN_BYTES;

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
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /// Remember a DAT or ISO path for this instance and later ones.
    void setKeyPath (const juce::String& path);
    /// Remember a mods folder for this instance and later ones.
    void setModsFolder (const juce::String& path);

    juce::AudioProcessorValueTreeState apvts;
    OrbModloader* engine() noexcept { return engine_.get(); }

    static const char* auxName (int index);
    static juce::String p808Id (int ctrl);
    static juce::String p909Id (int ctrl);
    static juce::String directId (int aux);

private:
    struct EngineDeleter
    {
        void operator() (OrbModloader* p) const noexcept { orb_modloader_free (p); }
    };

    static juce::AudioProcessor::BusesProperties createBuses();
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void processBuffer (juce::AudioBuffer<float>& buffer, bool bypass);
    OrbModloaderParams readParams (bool bypass) const;
    OrbModloaderPlayhead readPlayhead (int nHost);
    void clearPlayClock();
    int latencySamples() const;
    void engineToHost (float* const* hostOut, int nHost, int nEngine, int nCh);
    void shiftFifo (juce::AudioBuffer<float>& fifo, int& fill, int used);
    int asInt (const juce::String& id, int fallback) const;

    std::unique_ptr<OrbModloader, EngineDeleter> engine_;
    double hostRate = 44100.0;
    bool exact44100 = true;
    int maxHostBlock = 0;

    static constexpr int kTotalCh = 2 + kAuxBuses;
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
