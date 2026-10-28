#include "ErmDriver.h"
#include "AdafruitErmOutput.h"
#include "HapticOutput.h"
#include "UsbFrameCodec.h"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace mixsense;
namespace
{
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a - b) < 0.0001f; }
TransportMessage level(float value) { return {MessageKind::Level, 0, value}; }
TransportMessage pulse(float value, uint32_t ms) { return {MessageKind::Pulse, 0, value, FeedbackTarget::Haptic, ms}; }

void stateTests()
{
    bench::HapticOutput state;
    state.accept(level(0.4f), 0); state.accept(pulse(0.9f, 100), 10);
    state.accept(level(0.6f), 20);
    require(near(state.level(109), 0.9f), "level refresh must not truncate pulse");
    require(near(state.level(110), 0.6f), "pulse resumes latest baseline");
    state.accept(pulse(0.8f, 100), 120); state.accept(level(0), 130);
    require(near(state.level(219), 0.8f) && state.level(220) == 0, "zero clears baseline, not pulse");
    state.accept(pulse(0.7f, 2000), 300);
    require(near(state.level(799), 0.7f) && state.level(800) == 0, "bench pulse cap is 500ms");
    state.accept(pulse(0.9f, 100), 900); state.accept(pulse(0.5f, 50), 920);
    require(near(state.level(969), 0.5f) && state.level(970) == 0, "new pulse preempts old");
    state.accept(level(0.6f), 1000);
    auto visual = level(1); visual.target = FeedbackTarget::Visual;
    require(!state.accept(visual, 1900), "visual must not renew haptic watchdog");
    require(!state.accept(pulse(1, 0), 1901), "zero-duration pulse rejected");
    require(!state.accept(level(std::numeric_limits<float>::quiet_NaN()), 1902), "NaN rejected");
    require(near(state.level(1999), 0.6f) && state.level(2000) == 0, "watchdog deadline");
    state.accept(level(0.8f), 3000);
    state.accept(pulse(0.5f, 100), 4500); // Simulate a long main-loop gap.
    require(near(state.level(4599), 0.5f) && state.level(4600) == 0, "new pulse cannot revive expired baseline");
    state.accept(level(1), 5000); state.reset();
    require(state.level(5001) == 0, "disconnect cancels state and pulse");
    state.accept(level(1), 0xfffffff0u);
    require(state.level(0x3d7u) == 1 && state.level(0x3d8u) == 0, "watchdog across millis wrap");
}

struct FakeBus
{
    std::array<uint8_t, 256> reg {};
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    bool available = true, dropClampWrite = false;
    FakeBus() { reg[0] = 0xe0; reg[0x1a] = 0xb6; reg[0x1c] = 0x75; reg[0x1d] = 0x98; }
    bool read(uint8_t r, uint8_t& value) { value = reg[r]; return available; }
    bool write(uint8_t r, uint8_t value)
    {
        writes.emplace_back(r, value);
        if (!available) return false;
        if (!(dropClampWrite && r == 0x17)) reg[r] = value;
        return true;
    }
};

void driverTests()
{
    FakeBus bus;
    bench::ErmDriver<FakeBus> driver(bus);
    require(driver.begin(), "initialize ERM configuration with readback");
    require(bus.reg[0x17] == 0x70 && !(bus.reg[0x1a] & 0x80)
        && (bus.reg[0x1d] & 0x3f) == 0x20 && bus.reg[0x02] == 0, "ERM, voltage limit, signed neutral");
    for (auto [r, value] : bus.writes) require(r != 2 || value == 0, "initialization must never energize motor");
    using Driver = bench::ErmDriver<FakeBus>;
    require(Driver::positiveSignedRtp(0) == 0 && Driver::positiveSignedRtp(0.5f) == 64
        && Driver::positiveSignedRtp(1) == 127 && Driver::positiveSignedRtp(-1) == 0
        && Driver::positiveSignedRtp(2) == 127, "positive signed RTP mapping");
    driver.service(1, 10); require(bus.reg[2] == 127, "full logical level stays positive signed");
    driver.service(0, 11); require(bus.reg[2] == 0, "zero stops electrical drive");
    driver.service(1, 20); bus.available = false; driver.service(0, 21);
    require(!driver.ready(), "I2C fault latched");
    bus.available = true; driver.service(1, 200);
    require(!driver.ready() && bus.reg[2] == 0 && bus.reg[1] == 0x45, "recovery retries stop, never stale intensity");
    FakeBus readbackFailure; readbackFailure.dropClampWrite = true;
    Driver bad(readbackFailure);
    require(!bad.begin() && readbackFailure.reg[2] == 0, "failed voltage-limit readback blocks startup");
    FakeBus resetBus; Driver resetDriver(resetBus); require(resetDriver.begin(), "reset scenario init");
    resetDriver.service(1, 1); resetBus.reg[1] = 0x40; resetDriver.service(1, 100);
    require(!resetDriver.ready() && resetBus.reg[2] == 0, "driver reset stops rather than replays");
    FakeBus faultBus; Driver faultDriver(faultBus); require(faultDriver.begin(), "fault scenario init");
    faultBus.reg[0] |= 2; faultDriver.service(1, 100);
    require(!faultDriver.ready() && faultBus.reg[2] == 0, "overcurrent indication stops drive");
}

void protocolTests()
{
    bench::HapticOutput state;
    UsbStreamParser parser(FrameDirection::HostToDevice);
    // Known v1 50% haptic frame, delivered one byte at a time.
    const uint8_t bytes[] {0xaa, 0x55, 1, 0x10, 0, 2, 0, 0x80};
    for (auto byte : bytes) parser.feed(&byte, 1, [&](const auto& message) { return state.accept(message, 0); });
    require(near(state.level(0), 32768.0f / 65535), "fragmented wire frame reaches output state");
    const uint8_t malformed[] {0xaa, 0x55, 1, 0x10, 0, 0xff};
    parser.feed(malformed, sizeof(malformed), [&](const auto& message) { return state.accept(message, 900); });
    require(state.level(1000) == 0, "malformed traffic cannot keep motor running");
}

struct FakeAdafruit
{
    FakeBus& bus;
    bool begin() { bus.reg[0x1d] |= 0x20; return bus.available; }
    void useERM() { bus.reg[0x1a] &= 0x7f; }
    void setRealtimeValue(uint8_t value) { bus.write(2, value); }
    void setMode(uint8_t mode) { bus.write(1, mode); }
};
void adafruitOutputTests()
{
    FakeBus bus; FakeAdafruit library {bus};
    bus.reg[0x1d] = 0xa0; // DRV2605L reset Control3, signed RTP.
    bench::AdafruitErmOutput<FakeAdafruit, FakeBus> driver(library, bus);
    require(driver.begin() && bus.reg[2] == 0, "Adafruit initialization starts at zero");
    driver.service(1, 1); require(bus.reg[2] == 90, "combined firmware caps all requests at RTP 90");
    driver.service(0, 2); require(bus.reg[2] == 0, "zero immediately clears motor drive");
    driver.service(0.5f, 3); require(bus.reg[2] == 64, "signed RTP matches normalized protocol");
    bus.reg[1] = 0x40; driver.service(0.5f, 101);
    require(!driver.ready() && bus.reg[2] == 0, "driver reset latches stopped state");
    require(std::string(driver.faultReason()) == "MODE_not_RTP", "diagnostic preserves the reset fault reason");
    bus.reg[1] = 5; driver.service(1, 300);
    require(!driver.ready() && bus.reg[2] == 0, "recovery never replays a stale nonzero level");
    FakeBus failedBus; FakeAdafruit failedLibrary {failedBus};
    failedBus.reg[0x1d] = 0xa0;
    bench::AdafruitErmOutput<FakeAdafruit, FakeBus> failed(failedLibrary, failedBus);
    require(failed.begin(), "I2C failure scenario init");
    failedBus.available = false; failed.service(0.5f, 1);
    require(!failed.ready(), "write failure latches fault");
    require(std::string(failed.faultReason()) == "RTP_write_failed", "diagnostic identifies the failed output write");
}
}
int main()
{
    try { stateTests(); driverTests(); protocolTests(); adafruitOutputTests(); std::cout << "ERM state, Adafruit output, register safeguards and wire integration passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
