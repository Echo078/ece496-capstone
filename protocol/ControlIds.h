#pragma once

#include <cstdint>

namespace mixsense
{
// Stable logical identities shared with firmware; never pin/ADC assignments.
enum class ControlId : std::uint16_t
{
    Threshold = 0, Ratio = 1, Attack = 2, Release = 3, MakeupGain = 4, Bypass = 5
};
constexpr bool isKnownControl (ControlId id) noexcept
{
    switch (id)
    {
        case ControlId::Threshold: case ControlId::Ratio: case ControlId::Attack:
        case ControlId::Release: case ControlId::MakeupGain: case ControlId::Bypass:
            return true;
    }
    return false;
}
enum class FeedbackTarget { Haptic = 0, Visual = 1 };
}
