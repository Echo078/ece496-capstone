#include "ControllerParameterAdapter.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace mixsense
{
namespace
{
struct ParameterDefinition
{
    ControlId control;
    const char* id;
    const char* name;
    const char* unit;
    float minimum, maximum, initial, interval, skew;
};
// Provisional application ranges; never used by the transport or middleware.
constexpr std::array<ParameterDefinition, 6> definitions {{
    { ControlId::Threshold, "threshold", "Threshold", "dB", -60.0f, 0.0f, -24.0f, 0.01f, 1.0f },
    { ControlId::Ratio, "ratio", "Ratio", ":1", 1.0f, 20.0f, 4.0f, 0.01f, 1.0f },
    { ControlId::Attack, "attack", "Attack", "ms", 0.1f, 200.0f, 10.0f, 0.1f, 0.4f },
    { ControlId::Release, "release", "Release", "ms", 10.0f, 2000.0f, 100.0f, 1.0f, 0.4f },
    { ControlId::MakeupGain, "makeupGain", "Makeup Gain", "dB", -24.0f, 24.0f, 0.0f, 0.01f, 1.0f },
    { ControlId::Bypass, "bypass", "Bypass", "", 0.0f, 1.0f, 0.0f, 1.0f, 1.0f }
}};
}

const char* ControllerParameterAdapter::parameterId (ControlId control) noexcept
{
    for (const auto& definition : definitions)
        if (definition.control == control) return definition.id;
    return nullptr;
}

juce::AudioProcessorValueTreeState::ParameterLayout ControllerParameterAdapter::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& definition : definitions)
    {
        const juce::ParameterID id { definition.id, 1 };
        if (definition.control == ControlId::Bypass)
            layout.add (std::make_unique<juce::AudioParameterBool> (id, definition.name, false));
        else
            layout.add (std::make_unique<juce::AudioParameterFloat> (
                id, definition.name,
                juce::NormalisableRange<float> { definition.minimum, definition.maximum, definition.interval, definition.skew },
                definition.initial, juce::AudioParameterFloatAttributes().withLabel (definition.unit)));
    }
    return layout;
}

bool ControllerParameterAdapter::apply (const ControllerEvent& event)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto* id = parameterId (event.control);
    auto* parameter = id != nullptr ? parameters.getParameter (id) : nullptr;
    if (parameter == nullptr) return false;
    if (const auto* snapshot = std::get_if<InitialControlState> (&event.change))
        return std::isfinite (snapshot->normalized); // No host notification or takeover.
    float next = parameter->getValue();
    if (const auto* value = std::get_if<ContinuousValue> (&event.change))
    {
        if (! std::isfinite (value->normalized)) return false;
        next = std::clamp (value->normalized, 0.0f, 1.0f);
    }
    else
    {
        switch (std::get<DiscreteAction> (event.change))
        {
            case DiscreteAction::ButtonPressed:
                if (event.control != ControlId::Bypass) return false;
                next = next < 0.5f ? 1.0f : 0.0f;
                break;
            case DiscreteAction::ButtonReleased: return true; // Toggle on press only.
            case DiscreteAction::Increment: next += 0.01f; break;
            case DiscreteAction::Decrement: next -= 0.01f; break;
            default: return false;
        }
        if (event.control == ControlId::Bypass)
        {
            const auto action = std::get<DiscreteAction> (event.change);
            if (action == DiscreteAction::Increment) next = 1.0f;
            if (action == DiscreteAction::Decrement) next = 0.0f;
        }
    }
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost (std::clamp (next, 0.0f, 1.0f));
    parameter->endChangeGesture();
    takenOver[static_cast<std::size_t> (event.control)] = true;
    return true;
}

bool ControllerParameterAdapter::hasPhysicalTakeover (ControlId control) const noexcept
{
    return isKnownControl (control) && takenOver[static_cast<std::size_t> (control)];
}
}
