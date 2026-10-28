#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ControllerTransport.h"

namespace mixsense::bench {
// Bench policy: newest pulse preempts; pulses capped at 500ms. Level updates,
// including zero, change the post-pulse baseline without truncating the pulse.
class HapticOutput {
 public:
  static constexpr uint32_t watchdogMs = 1000, maxPulseMs = 500;
  bool accept(const TransportMessage& message, uint32_t now) {
    if (message.target != FeedbackTarget::Haptic || message.id != 0 ||
        !std::isfinite(message.value))
      return false;
    if (message.kind != MessageKind::Level &&
        message.kind != MessageKind::Pulse)
      return false;
    if (message.kind == MessageKind::Pulse && message.durationMs == 0)
      return false;
    level(now);  // Expire old state before a new command renews the lease.
    const float value = std::clamp(message.value, 0.0f, 1.0f);
    if (message.kind == MessageKind::Level)
      baseline = value;
    else {
      pulseLevel = value;
      pulseStarted = now;
      pulseDuration = std::min(message.durationMs, maxPulseMs);
      pulseActive = true;
    }
    lastCommand = now;
    leased = true;
    return true;
  }
  float level(uint32_t now) {
    if (!leased || now - lastCommand >= watchdogMs) {
      reset();
      return 0.0f;
    }
    if (pulseActive && now - pulseStarted >= pulseDuration) pulseActive = false;
    return pulseActive ? pulseLevel : baseline;
  }
  void reset() {
    baseline = pulseLevel = 0.0f;
    pulseActive = leased = false;
  }

 private:
  float baseline = 0.0f, pulseLevel = 0.0f;
  uint32_t lastCommand = 0, pulseStarted = 0, pulseDuration = 0;
  bool leased = false, pulseActive = false;
};
}  // namespace mixsense::bench
