#include "ControllerBridge.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace mixsense
{
std::optional<ControllerEvent> LogicalMessageCodec::decode (const TransportMessage& message)
{
    const auto control = static_cast<ControlId> (message.id);
    if (! isKnownControl (control)) return std::nullopt;
    switch (message.kind)
    {
        case MessageKind::Value:
            if (! std::isfinite (message.value)) return std::nullopt;
            return ControllerEvent { control, ContinuousValue { std::clamp (message.value, 0.0f, 1.0f) } };
        case MessageKind::Snapshot:
            if (! std::isfinite (message.value)) return std::nullopt;
            return ControllerEvent { control, InitialControlState { std::clamp (message.value, 0.0f, 1.0f) } };
        case MessageKind::Press: return ControllerEvent { control, DiscreteAction::ButtonPressed };
        case MessageKind::Release: return ControllerEvent { control, DiscreteAction::ButtonReleased };
        case MessageKind::Increment: return ControllerEvent { control, DiscreteAction::Increment };
        case MessageKind::Decrement: return ControllerEvent { control, DiscreteAction::Decrement };
        default: return std::nullopt;
    }
}

std::optional<FeedbackCommand> LogicalMessageCodec::validate (FeedbackCommand command)
{
    if (command.address.target != FeedbackTarget::Haptic && command.address.target != FeedbackTarget::Visual)
        return std::nullopt;
    const bool valid = std::visit ([] (auto& payload)
    {
        if (! std::isfinite (payload.normalized)) return false;
        payload.normalized = std::clamp (payload.normalized, 0.0f, 1.0f);
        return true;
    }, command.payload);
    if (! valid) return std::nullopt;
    if (const auto* pulse = std::get_if<FeedbackPulse> (&command.payload); pulse != nullptr && pulse->durationMs == 0)
        return std::nullopt;
    return command;
}

TransportMessage LogicalMessageCodec::encode (const FeedbackCommand& command)
{
    if (const auto* level = std::get_if<FeedbackLevel> (&command.payload))
        return { MessageKind::Level, command.address.id, level->normalized, command.address.target, 0 };
    const auto& pulse = std::get<FeedbackPulse> (command.payload);
    return { MessageKind::Pulse, command.address.id, pulse.normalized, command.address.target, pulse.durationMs };
}

ControllerBridge::ControllerBridge (IControllerTransport& device, EventSink eventSink, SessionSink sessionCallback)
    : transport (device), sink (std::move (eventSink)), sessionSink (std::move (sessionCallback)) {}

void ControllerBridge::clearPending()
{
    levels.clear(); pulses.clear(); lastFeedback.reset();
}

void ControllerBridge::synchronizeSession()
{
    const auto token = transport.sessionId();
    const bool connected = isConnected();
    if (token == observedSession && connected == observedConnected) return;
    observedSession = token; observedConnected = connected;
    clearPending(); ++epoch;
    if (sessionSink) sessionSink();
}

std::uint64_t ControllerBridge::sessionGeneration() { synchronizeSession(); return epoch; }

bool ControllerBridge::connect()
{
    synchronizeSession();
    if (isConnected()) return true;
    // Also handle reconnects where no service call observed the lost session.
    clearPending();
    const bool accepted = transport.connect();
    synchronizeSession();
    return accepted;
}
bool ControllerBridge::isConnected() const { return transport.isConnected(); }
void ControllerBridge::disconnect()
{
    transport.disconnect();
    clearPending();
    synchronizeSession();
}

bool ControllerBridge::submitFeedback (FeedbackCommand command)
{
    synchronizeSession();
    auto validated = LogicalMessageCodec::validate (std::move (command));
    if (! validated || ! isConnected() || ! transport.supports (LogicalMessageCodec::encode (*validated))) return false;
    if (std::holds_alternative<FeedbackPulse> (validated->payload))
    {
        if (pulses.size() >= pendingCapacity) return false;
        pulses.push_back (*validated);
    }
    else
    {
        auto existing = std::find_if (levels.begin(), levels.end(), [&] (const auto& level)
        { return level.address == validated->address; });
        if (existing == levels.end())
        {
            if (levels.size() >= pendingCapacity) return false;
            levels.push_back (*validated);
        }
        else *existing = *validated;
    }
    return true;
}

void ControllerBridge::setFeedbackInterval (Clock::duration interval)
{
    feedbackInterval = std::max (interval, Clock::duration::zero());
}

void ControllerBridge::service (Clock::time_point now, std::size_t receiveBudget)
{
    synchronizeSession();
    if (! isConnected())
    {
        // A lost session cancels pending output; never replay old pulses on reconnect.
        clearPending();
        return;
    }
    for (std::size_t i = 0; i < receiveBudget; ++i)
    {
        const auto message = transport.receive();
        if (! message) break;
        if (const auto event = LogicalMessageCodec::decode (*message); event && sink) sink (*event);
    }
    // A safety zero must not wait behind continuous rate limiting. It updates
    // the firmware baseline; it does not cancel an independently timed pulse.
    for (auto it = levels.begin(); it != levels.end();)
    {
        const auto* level = std::get_if<FeedbackLevel> (&it->payload);
        if (it->address.target == FeedbackTarget::Haptic && level && level->normalized == 0.0f)
        {
            if (! transport.send (LogicalMessageCodec::encode (*it))) return;
            it = levels.erase (it);
        }
        else ++it;
    }
    // Normal pulses are serviced first; firmware owns override timing and must
    // retain subsequent level commands as the latest post-pulse baseline.
    while (! pulses.empty())
    {
        if (! transport.send (LogicalMessageCodec::encode (pulses.front()))) return;
        pulses.pop_front();
    }
    if (lastFeedback && now - *lastFeedback < feedbackInterval) return;
    bool sentAny = false;
    while (! levels.empty())
    {
        if (! transport.send (LogicalMessageCodec::encode (levels.front()))) break;
        levels.erase (levels.begin());
        sentAny = true;
    }
    if (sentAny) lastFeedback = now;
}
}
