#pragma once

#include "ControllerTypes.h"
#include <deque>
#include <optional>
#include <vector>

namespace mixsense
{
// In-process envelope, never a packed wire layout. Raw identifiers let
// the codec exercise validation of untrusted transport data.
enum class MessageKind { Value, Press, Release, Increment, Decrement, Level, Pulse, Snapshot };
struct TransportMessage
{
    MessageKind kind;
    std::uint16_t id;
    float value = 0.0f;
    FeedbackTarget target = FeedbackTarget::Haptic;
    std::uint32_t durationMs = 0;
};

class IControllerTransport
{
public:
    virtual ~IControllerTransport() = default;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    // Empty receive means no message available; false send means not accepted.
    virtual std::optional<TransportMessage> receive() = 0;
    virtual bool send (const TransportMessage&) = 0;
    // Capability check before queuing; the in-memory envelope accepts all types.
    virtual bool supports (const TransportMessage&) const { return true; }
    // Changes on each local session transition, even if the owner did not observe
    // a brief disconnect. Decorators should forward this token.
    virtual std::uint64_t sessionId() const noexcept { return 0; }
};

// Single-owner mock: injection and inspection happen on the same servicing thread.
class InMemoryControllerTransport final : public IControllerTransport
{
public:
    bool connect() override { if (! connected) ++session; connected = true; return true; }
    void disconnect() override { ++session; connected = false; incoming.clear(); }
    std::uint64_t sessionId() const noexcept override { return session; }
    bool isConnected() const override { return connected; }
    std::optional<TransportMessage> receive() override
    {
        if (! connected || incoming.empty()) return std::nullopt;
        const auto message = incoming.front();
        incoming.pop_front();
        return message;
    }
    bool send (const TransportMessage& message) override
    {
        if (! connected) return false;
        sent.push_back (message);
        return true;
    }
    bool inject (TransportMessage message)
    {
        if (! connected) return false;
        incoming.push_back (message);
        return true;
    }
    const std::vector<TransportMessage>& sentMessages() const { return sent; }
private:
    bool connected = false;
    std::uint64_t session = 0;
    std::deque<TransportMessage> incoming;
    std::vector<TransportMessage> sent;
};
}
