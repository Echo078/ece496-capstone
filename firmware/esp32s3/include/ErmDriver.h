#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mixsense::bench
{
// Bus supplies bool read(reg, value) / write(reg, value). No external library.
// TI DRV2605L Rev D, Eq.6: ERM open-loop average voltage = 21.59mV * OD_CLAMP.
// 0x70 -> nominal 2.418V at full positive signed RTP, below the listed 3V rating.
template<class Bus> class ErmDriver
{
public:
    static constexpr uint8_t clampRegister = 0x70;
    explicit ErmDriver(Bus& registers) : bus(registers) {}
    bool begin()
    {
        healthy = identified = false;
        uint8_t status = 0, feedback = 0, control2 = 0, control3 = 0;
        if (!bus.read(0x00, status) || (status >> 5) != 7) return false;
        identified = true;
        // Park output before changing configuration. No auto-calibration or ROM effects.
        if (!bus.write(0x01, 0x40) || !bus.write(0x0c, 0) || !bus.write(0x02, 0)
            || !bus.read(0x1a, feedback) || !bus.read(0x1c, control2) || !bus.read(0x1d, control3))
            return fail();
        feedback &= 0x7f;               // ERM, not LRA.
        control2 |= 0x80;              // Bidirectional input required for open-loop.
        control3 = (control3 & 0xc0) | 0x20; // ERM open-loop, signed RTP, supply compensation.
        if (!bus.write(0x17, clampRegister) || !bus.write(0x1a, feedback)
            || !bus.write(0x1c, control2) || !bus.write(0x1d, control3)
            || !matches(0x17, clampRegister) || !matches(0x1a, feedback)
            || !matches(0x1c, control2) || !matches(0x1d, control3)
            || !bus.write(0x01, 0x05) || !matches(0x01, 0x05)
            || !bus.read(0x00, status) || (status & 0x03)) return fail();
        healthy = true; lastRtp = 0; lastWrite = lastPoll = 0;
        return true;
    }
    static uint8_t positiveSignedRtp(float level)
    {
        if (!std::isfinite(level)) return 0;
        // Signed RTP: 0 is neutral; only positive 0..127, never reverse drive.
        return static_cast<uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 127.0f));
    }
    void service(float level, uint32_t now)
    {
        if (!healthy)
        {
            // Latch failure, never restart an old request after I2C/power recovers.
            if (identified && now - lastWrite >= 100) { stopBestEffort(); lastWrite = now; }
            return;
        }
        if (now - lastPoll >= 100)
        {
            lastPoll = now;
            uint8_t status = 0;
            if (!bus.read(0x00, status) || (status & 0x03) || (status >> 5) != 7
                || !matches(0x01, 0x05) || !matches(0x17, clampRegister)
                || !bitsMatch(0x1a, 0x80, 0) || !bitsMatch(0x1c, 0x80, 0x80)
                || !bitsMatch(0x1d, 0x3f, 0x20)) { fail(); return; }
        }
        const uint8_t next = positiveSignedRtp(level);
        if (next != lastRtp || now - lastWrite >= 100)
        {
            if (!bus.write(0x02, next)) { fail(); return; }
            lastRtp = next; lastWrite = now;
        }
    }
    bool ready() const { return healthy; }
private:
    bool matches(uint8_t reg, uint8_t expected)
    {
        uint8_t actual = 0;
        return bus.read(reg, actual) && actual == expected;
    }
    bool bitsMatch(uint8_t reg, uint8_t mask, uint8_t expected)
    {
        uint8_t actual = 0;
        return bus.read(reg, actual) && (actual & mask) == expected;
    }
    void stopBestEffort() { bus.write(0x02, 0); bus.write(0x01, 0x45); }
    bool fail() { healthy = false; stopBestEffort(); return false; }
    Bus& bus;
    bool healthy = false, identified = false;
    uint8_t lastRtp = 0;
    uint32_t lastWrite = 0, lastPoll = 0;
};
}
