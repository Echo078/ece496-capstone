#pragma once
#include "GrHapticMapping.h"
#include <cstdint>

namespace mixsense::bench
{
// Preserve the proven Adafruit initialization; check subsequent bus transfers.
// Library: begin/useERM/setRealtimeValue/setMode. Bus: bool read/write.
template<class Library, class Bus> class AdafruitErmOutput
{
public:
    AdafruitErmOutput(Library& library, Bus& registers) : drv(library), bus(registers) {}
    bool begin()
    {
        healthy = identified = false;
        fault = "none";
        if (!drv.begin()) { fault = "Adafruit_begin_not_detected"; return false; }
        identified = true;
        drv.useERM(); drv.setRealtimeValue(0); drv.setMode(5);
        if (!configurationOk()) return fail();
        healthy = true; lastRtp = 0; lastWrite = lastPoll = 0;
        return true;
    }
    bool ready() const { return healthy; }
    const char* faultReason() const { return fault; }
    void service(float requested, uint32_t now)
    {
        if (!healthy)
        {
            if (identified && now - lastWrite >= 100) { stop(); lastWrite = now; }
            return;
        }
        if (now - lastPoll >= 100)
        {
            lastPoll = now;
            if (!configurationOk()) { fail(); return; }
        }
        const auto next = grhaptic::boundedRtp(requested);
        if (next != lastRtp || now - lastWrite >= 100)
        {
            if (!bus.write(0x02, next)) { fault = "RTP_write_failed"; fail(); return; }
            lastRtp = next; lastWrite = now;
        }
    }
private:
    bool configurationOk()
    {
        uint8_t status = 0, mode = 0, feedback = 0, control3 = 0;
        if (!bus.read(0x00, status)) { fault = "STATUS_read_failed"; return false; }
        if ((status >> 5) != 7) { fault = "unexpected_device_ID"; return false; }
        if (status & 3) { fault = "driver_protection_fault"; return false; }
        if (!bus.read(0x01, mode)) { fault = "MODE_read_failed"; return false; }
        if (mode != 5) { fault = "MODE_not_RTP"; return false; }
        if (!bus.read(0x1a, feedback)) { fault = "FEEDBACK_read_failed"; return false; }
        if (feedback & 0x80) { fault = "driver_in_LRA_mode"; return false; }
        if (!bus.read(0x1d, control3)) { fault = "CONTROL3_read_failed"; return false; }
        if ((control3 & 0x28) != 0x20) { fault = "RTP_format_or_open_loop"; return false; }
        return true;
    }
    void stop() { bus.write(0x02, 0); bus.write(0x01, 0x45); }
    bool fail() { healthy = false; stop(); return false; }
    Library& drv;
    Bus& bus;
    bool healthy = false, identified = false;
    const char* fault = "not_initialized";
    uint8_t lastRtp = 0;
    uint32_t lastWrite = 0, lastPoll = 0;
};
}
