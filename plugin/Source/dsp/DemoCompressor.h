#pragma once

#include <algorithm>
#include <cmath>

namespace mixsense
{
// Prototype hard-knee, stereo-linked feed-forward compressor. Reduction is
// smoothed in dB; the same applied gain drives audio and the GR measurement.
// All methods belong to the audio thread. No allocation, locks or device I/O.
class DemoCompressor
{
public:
    struct Parameters
    {
        float thresholdDb = -24.0f, ratio = 4.0f;
        float attackMs = 10.0f, releaseMs = 100.0f, makeupDb = 0.0f;
    };
    struct Result { float maximumReductionDb = 0.0f, inputPeak = 0.0f; };

    void prepare(double rate) noexcept
    {
        sampleRate = std::isfinite(rate) && rate > 0.0 ? rate : 48000.0;
        reset();
    }
    void reset() noexcept { reductionDb = 0.0f; }
    Result process(float* const* channels, int channelCount, int samples,
                   Parameters settings, bool bypassed) noexcept
    {
        Result result;
        if (channelCount <= 0 || samples <= 0) return result;
        const float threshold = finite(settings.thresholdDb, -24.0f, -60.0f, 0.0f);
        const float ratio = finite(settings.ratio, 4.0f, 1.0f, 20.0f);
        const float attack = coefficient(finite(settings.attackMs, 10.0f, 0.1f, 200.0f));
        const float release = coefficient(finite(settings.releaseMs, 100.0f, 10.0f, 2000.0f));
        const float makeup = std::pow(10.0f, finite(settings.makeupDb, 0.0f, -24.0f, 24.0f) / 20.0f);
        if (bypassed) reset();

        for (int sample = 0; sample < samples; ++sample)
        {
            float peak = 0.0f;
            for (int channel = 0; channel < channelCount; ++channel)
                peak = std::max(peak, std::abs(channels[channel][sample]));
            result.inputPeak = std::max(result.inputPeak, peak);
            if (bypassed) continue; // Bit-exact dry audio, including ignoring makeup.
            const float inputDb = 20.0f * std::log10(std::max(peak, 1.0e-9f));
            const float target = std::max(0.0f, (inputDb - threshold) * (1.0f - 1.0f / ratio));
            const float smoothing = target > reductionDb ? attack : release;
            reductionDb = target + smoothing * (reductionDb - target);
            if (reductionDb < 1.0e-6f) reductionDb = 0.0f;
            const float gain = std::pow(10.0f, -reductionDb / 20.0f) * makeup;
            for (int channel = 0; channel < channelCount; ++channel)
                channels[channel][sample] *= gain;
            result.maximumReductionDb = std::max(result.maximumReductionDb, reductionDb);
        }
        return result;
    }
private:
    static float finite(float value, float fallback, float low, float high) noexcept
    { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; }
    float coefficient(float ms) const noexcept
    { return static_cast<float>(std::exp(-1.0 / (0.001 * ms * sampleRate))); }
    double sampleRate = 48000.0;
    float reductionDb = 0.0f;
};
}
