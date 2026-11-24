#pragma once

#include <JuceHeader.h>
#include "orb_modloader.h"

#include <cstdint>
#include <optional>

/// MIDI note map for dumping a drum pattern to a DAW track and playing it back.
/// 808 instruments occupy C1–A#1; 909 instruments occupy C2–A#2. Accent is velocity, not a note.
namespace patternmidi
{
constexpr int kPpq = 192;
constexpr int kStepTicks = 48;
constexpr int kBarTicks = 16 * kStepTicks;
constexpr int kPatterns = 32;
constexpr int kShufflePeriod = 96;
constexpr int kShuffleDiv = 6;
constexpr int kGateTicks = 24;
constexpr int kFlamDelayTicks = 12;
constexpr int kPatternLen = 194;
constexpr int kDrumCols = 11;
constexpr int k808BaseNote = 36;
constexpr int k909BaseNote = 48;
constexpr uint8_t kCtrlEnable = 0;
constexpr uint8_t kCtrlPattern = 1;
constexpr juce::uint8 kVelOn = 100;
constexpr juce::uint8 kVelAccent = 127;
/// 909 step value 1/3 with AC off. Double-click (2/4) and the AC column are independent
/// multipliers in ReBirth; MIDI has to encode all four combinations below 127.
constexpr juce::uint8 k909VelOn = 80;
constexpr juce::uint8 k909VelOnAc = 104;
constexpr juce::uint8 k909VelHard = 112;
constexpr juce::uint8 k909VelHardAc = 127;

constexpr const char* k808Names[] = {
    "808 BD", "808 SD", "808 LT", "808 MT", "808 HT",
    "808 RS", "808 CP", "808 CB", "808 CY", "808 OH", "808 CH"
};
constexpr const char* k909Names[] = {
    "909 BD", "909 SD", "909 LT", "909 MT", "909 HT",
    "909 RS", "909 CP", "909 CH", "909 OH", "909 CC", "909 RC"
};

inline int baseNote (int machine) noexcept
{
    return machine == 0 ? k808BaseNote : k909BaseNote;
}

inline int drumNote (int machine, int col) noexcept
{
    if (col < 1 || col > kDrumCols)
        return -1;
    return baseNote (machine) + (col - 1);
}

inline bool noteToDrum (int note, int& machine, int& col) noexcept
{
    if (note >= k808BaseNote && note < k808BaseNote + kDrumCols)
    {
        machine = 0;
        col = note - k808BaseNote + 1;
        return true;
    }
    if (note >= k909BaseNote && note < k909BaseNote + kDrumCols)
    {
        machine = 1;
        col = note - k909BaseNote + 1;
        return true;
    }
    return false;
}

inline std::optional<juce::String> nameForNote (int note)
{
    int machine = 0, col = 0;
    if (! noteToDrum (note, machine, col))
        return std::nullopt;
    const auto* names = machine == 0 ? k808Names : k909Names;
    return juce::String (names[col - 1]);
}

inline int shuffledTick (int tick, uint8_t shuffle) noexcept
{
    const int s = (int) shuffle / kShuffleDiv;
    const int r = juce::jmax (0, tick % kShufflePeriod - s);
    return kShufflePeriod * (tick / kShufflePeriod) + r;
}

inline juce::uint8 velocityForStep (int machine, uint8_t value, bool accent) noexcept
{
    if (machine == 1)
    {
        const bool hard = value == 2 || value == 4;
        if (hard)
            return accent ? k909VelHardAc : k909VelHard;
        return accent ? k909VelOnAc : k909VelOn;
    }
    return accent ? kVelAccent : kVelOn;
}

inline void addHits (juce::MidiMessageSequence& seq, int machine, const uint8_t* row, int tick)
{
    const bool accent = row[0] != 0;

    for (int col = 1; col <= kDrumCols; ++col)
    {
        const uint8_t value = row[col];
        if (value == 0)
            continue;

        const int note = drumNote (machine, col);
        const auto vel = velocityForStep (machine, value, accent);
        seq.addEvent (juce::MidiMessage::noteOn (1, note, vel), (double) tick);
        seq.addEvent (juce::MidiMessage::noteOff (1, note), (double) (tick + kGateTicks));

        if (machine == 1 && value >= 3)
        {
            const int flamAt = tick + kFlamDelayTicks;
            seq.addEvent (juce::MidiMessage::noteOn (1, note, vel), (double) flamAt);
            seq.addEvent (juce::MidiMessage::noteOff (1, note), (double) (flamAt + kGateTicks));
        }
    }
}

inline juce::MidiFile fromPattern (int machine, const uint8_t* pat, uint8_t shuffleAmount)
{
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, machine == 0 ? "808" : "909"), 0.0);

    const int length = juce::jlimit (1, 16, (int) pat[1]);
    const bool shuffleOn = pat[0] != 0;

    for (int step = 0; step < length; ++step)
    {
        int tick = step * kStepTicks;
        if (shuffleOn)
            tick = shuffledTick (tick, shuffleAmount);
        addHits (seq, machine, pat + 2 + 12 * step, tick);
    }

    seq.updateMatchedPairs();
    juce::MidiFile file;
    file.setTicksPerQuarterNote (kPpq);
    file.addTrack (seq);
    return file;
}

inline void latchSongCtrls (const OrbModloaderTrackEvent* events, int eventCount,
                            int machine, int throughTick,
                            uint8_t& enable, uint8_t& pattern) noexcept
{
    if (events == nullptr || eventCount <= 0)
        return;

    for (int i = 0; i < eventCount; ++i)
    {
        const auto& e = events[i];
        if (e.tick > throughTick || (int) e.machine != machine)
            continue;
        if (e.ctrl == kCtrlEnable)
            enable = e.value != 0 ? 1 : 0;
        else if (e.ctrl == kCtrlPattern)
            pattern = (uint8_t) juce::jlimit (0, kPatterns - 1, (int) e.value);
    }
}

/// Chase Enable (ctrl 0) and Pattern (ctrl 1) over `[0, loopEndTick)`. Enable-off 16ths
/// emit nothing; Pattern still updates so the right slot is armed when Enable returns.
inline juce::MidiFile fromSong (int machine,
                                const uint8_t* bank,
                                int bankBytes,
                                const OrbModloaderTrackEvent* events,
                                int eventCount,
                                int loopEndTick,
                                uint8_t shuffleAmount)
{
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, machine == 0 ? "808" : "909"), 0.0);

    uint8_t enable = 1;
    uint8_t pattern = 0;
    latchSongCtrls (events, eventCount, machine, 0, enable, pattern);
    int patStart = 0;

    for (int tick = 0; tick < loopEndTick; tick += kStepTicks)
    {
        if (tick % kBarTicks == 0)
        {
            uint8_t nextEnable = enable;
            uint8_t nextPattern = pattern;
            latchSongCtrls (events, eventCount, machine, tick, nextEnable, nextPattern);
            if (nextPattern != pattern)
            {
                pattern = nextPattern;
                patStart = tick;
            }
            enable = nextEnable;
        }

        if (enable == 0 || bank == nullptr || bankBytes < kPatterns * kPatternLen)
            continue;

        const int slot = juce::jlimit (0, kPatterns - 1, (int) pattern);
        const uint8_t* pat = bank + slot * kPatternLen;
        const int length = juce::jlimit (1, 16, (int) pat[1]);
        const unsigned period = (unsigned) (kStepTicks * length);
        const unsigned v = (unsigned) (tick - patStart) % period;
        const int step = (int) (v / (unsigned) kStepTicks);
        int at = tick;
        if (pat[0] != 0)
            at = shuffledTick (tick, shuffleAmount);
        addHits (seq, machine, pat + 2 + 12 * step, at);
    }

    seq.updateMatchedPairs();
    if (loopEndTick > 0)
        seq.addEvent (juce::MidiMessage::endOfTrack(), (double) loopEndTick);

    juce::MidiFile file;
    file.setTicksPerQuarterNote (kPpq);
    file.addTrack (seq);
    return file;
}

inline bool writeFile (const juce::File& file, int machine, const uint8_t* pat, uint8_t shuffleAmount)
{
    auto midi = fromPattern (machine, pat, shuffleAmount);
    juce::FileOutputStream stream (file);
    if (! stream.openedOk())
        return false;
    stream.setPosition (0);
    stream.truncate();
    return midi.writeTo (stream);
}

inline bool writeSongFile (const juce::File& file,
                           int machine,
                           const uint8_t* bank,
                           int bankBytes,
                           const OrbModloaderTrackEvent* events,
                           int eventCount,
                           int loopEndTick,
                           uint8_t shuffleAmount)
{
    if (loopEndTick <= 0 || bank == nullptr || bankBytes < kPatterns * kPatternLen)
        return false;
    auto midi = fromSong (machine, bank, bankBytes, events, eventCount, loopEndTick, shuffleAmount);
    juce::FileOutputStream stream (file);
    if (! stream.openedOk())
        return false;
    stream.setPosition (0);
    stream.truncate();
    return midi.writeTo (stream);
}
} // namespace patternmidi
