#pragma once

#include "controller/ControllerBridge.h"

namespace mixsense
{
// Application-side lease/refresh for ONE continuous haptic endpoint. All methods
// run on the non-audio owner thread. No compressor units or musical mapping here.
class ContinuousFeedbackController
{
public:
    using Clock = ControllerBridge::Clock;
    struct Settings
    {
        Clock::duration refreshInterval;
        Clock::duration sourceMaxAge;
    };
    explicit ContinuousFeedbackController (ControllerBridge& bridge,
                                          FeedbackAddress address = { FeedbackTarget::Haptic, 0 });
    bool configure (Settings); // Both durations must be positive; no production defaults.
    // generation MUST advance only when the underlying source publishes new data,
    // not whenever a GUI/mapper observes the same cached metric. Repeated/older
    // generations never renew the source lease. Invalid input requests zero.
    // true accepts the desired publication; bridge backpressure is retried from
    // bounded state. This is not a transport acceptance/delivery acknowledgement.
    bool publish (float normalized, bool valid, std::uint64_t generation, Clock::time_point now);
    void clear (Clock::time_point now);
    void service (Clock::time_point now);
    bool active() const noexcept { return level.has_value(); }
private:
    void synchronizeSession();
    ControllerBridge& bridge;
    FeedbackAddress address;
    std::optional<Settings> settings;
    std::uint64_t session;
    std::optional<std::uint64_t> lastGeneration;
    std::optional<float> level;
    Clock::time_point sourceTime {};
    std::optional<Clock::time_point> lastAccepted;
    bool dirty = false, zeroPending = false;
};
}
