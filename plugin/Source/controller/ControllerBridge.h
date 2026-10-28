#pragma once

#include "ControllerTransport.h"
#include <chrono>
#include <functional>

namespace mixsense
{
// Validate logical transport envelopes independently of byte framing.
class LogicalMessageCodec
{
public:
    static std::optional<ControllerEvent> decode (const TransportMessage&);
    static std::optional<FeedbackCommand> validate (FeedbackCommand);
    static TransportMessage encode (const FeedbackCommand& validatedCommand);
};

class ControllerBridge
{
public:
    static constexpr std::size_t pendingCapacity = 128;
    using Clock = std::chrono::steady_clock;
    using EventSink = std::function<void (const ControllerEvent&)>;
    using SessionSink = std::function<void()>;

    // Transport must outlive bridge. All calls, including sink, belong to one
    // non-audio owner thread (JUCE message thread for the plugin adapter).
    ControllerBridge (IControllerTransport& transport, EventSink sink, SessionSink sessionSink = {});
    bool connect();
    void disconnect();
    bool isConnected() const;
    // Synchronizes session-local state before returning an application epoch.
    std::uint64_t sessionGeneration();
    // Rejected commands return false. Accepted pulses stay FIFO until sent or
    // explicitly cancelled by disconnect; levels coalesce by logical address.
    bool submitFeedback (FeedbackCommand);
    void setFeedbackInterval (Clock::duration interval);
    void service (Clock::time_point now = Clock::now(), std::size_t receiveBudget = 64);

private:
    IControllerTransport& transport;
    EventSink sink;
    SessionSink sessionSink;
    void synchronizeSession();
    void clearPending();
    bool observedConnected = false;
    std::uint64_t observedSession = 0, epoch = 0;
    std::vector<FeedbackCommand> levels;
    std::deque<FeedbackCommand> pulses;
    Clock::duration feedbackInterval {};
    std::optional<Clock::time_point> lastFeedback;
};
}
