#pragma once

#include "ControllerTypes.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

namespace mixsense
{
// The only mapping from logical controls to audio-domain parameters.
class ControllerParameterAdapter
{
public:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const char* parameterId (ControlId) noexcept;
    explicit ControllerParameterAdapter (juce::AudioProcessorValueTreeState& state) : parameters (state) {}
    // Call on JUCE's message thread, never processBlock or a transport worker.
    bool apply (const ControllerEvent&);
    void resetTakeoverSession() noexcept { takenOver.fill (false); }
    bool hasPhysicalTakeover (ControlId control) const noexcept;
private:
    juce::AudioProcessorValueTreeState& parameters;
    std::array<bool, 6> takenOver {};
};
}
