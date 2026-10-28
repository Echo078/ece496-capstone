#pragma once

#include <algorithm>
#include <cmath>

namespace mixsense::bench
{
// ADC values are calibration points, not forces in newtons.
class FsrInput
{
public:
    FsrInput(float released, float pressed, float movement = 0.02f)
        : low(released), high(pressed), deadband(movement) {}

    void sample(float raw)
    {
        if (!initialized) { filtered = raw; initialized = true; }
        else filtered += 0.15f * (raw - filtered);
    }
    float normalized() const
    {
        return high > low ? std::clamp((filtered - low) / (high - low), 0.0f, 1.0f) : 0.0f;
    }
    float filteredRaw() const { return filtered; }
    void baseline() { previous = normalized(); }
    bool moved() const { return std::abs(normalized() - previous) >= deadband; }

private:
    float low, high, deadband, filtered = 0.0f, previous = 0.0f;
    bool initialized = false;
};
}
