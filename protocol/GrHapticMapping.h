#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mixsense::grhaptic
{
// First bench mapping, not perceptually calibrated. Positive signed RTP only.
constexpr float deadbandDb = 0.5f, fullScaleDb = 18.0f;
constexpr uint8_t minimumRtp = 30, maximumRtp = 90;
inline float normalizedDrive(float reductionDb, float inputPeakDb) noexcept
{
    if (!std::isfinite(reductionDb) || !std::isfinite(inputPeakDb)
        || inputPeakDb < -90.0f || reductionDb <= deadbandDb) return 0.0f;
    const float amount = std::clamp((reductionDb - deadbandDb) / (fullScaleDb - deadbandDb), 0.0f, 1.0f);
    return (minimumRtp + amount * (maximumRtp - minimumRtp)) / 127.0f;
}
inline uint8_t boundedRtp(float normalized) noexcept
{
    if (!std::isfinite(normalized)) return 0;
    return static_cast<uint8_t>(std::lround(std::clamp(normalized, 0.0f,
        static_cast<float>(maximumRtp) / 127.0f) * 127.0f));
}
}
