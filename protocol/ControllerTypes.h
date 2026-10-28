#pragma once

#include "ControlIds.h"
#include <variant>

namespace mixsense
{
// Absolute position after genuine calibrated movement, never startup publication.
struct ContinuousValue { float normalized; };
// Informational absolute position, never a parameter edit (even after takeover).
struct InitialControlState { float normalized; };
enum class DiscreteAction { ButtonPressed, ButtonReleased, Increment, Decrement };
// Continuous values describe replaceable state; every discrete action is an event.
struct ControllerEvent
{
    ControlId control;
    std::variant<ContinuousValue, DiscreteAction, InitialControlState> change;
};

struct FeedbackAddress
{
    FeedbackTarget target;
    std::uint16_t id;
    friend bool operator== (FeedbackAddress a, FeedbackAddress b) noexcept
    {
        return a.target == b.target && a.id == b.id;
    }
};
// Persistent state may coalesce; timed pulses must retain their individual identity.
// Future properties can become additional typed payloads without changing transports.
struct FeedbackLevel { float normalized; };
struct FeedbackPulse { float normalized; std::uint32_t durationMs; };
struct FeedbackCommand
{
    FeedbackAddress address;
    std::variant<FeedbackLevel, FeedbackPulse> payload;
};
}
