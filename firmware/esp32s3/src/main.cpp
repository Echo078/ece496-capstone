#include <Arduino.h>
#include <tusb.h>
#include "BenchInput.h"
#include "HapticOutput.h"
#include "UsbFrameCodec.h"

#ifndef MIXSENSE_FSR_MONITOR
#define MIXSENSE_FSR_MONITOR 1
#endif
#ifndef MIXSENSE_I2C_CHECK
#define MIXSENSE_I2C_CHECK 0
#endif
#ifndef MIXSENSE_ENABLE_ERM
#define MIXSENSE_ENABLE_ERM 0
#endif
#ifndef MIXSENSE_MOTOR_TEST
#define MIXSENSE_MOTOR_TEST 0
#endif
#ifndef MIXSENSE_ADAFRUIT_ERM
#define MIXSENSE_ADAFRUIT_ERM 0
#endif
#ifndef MIXSENSE_FSR_INVERT
#define MIXSENSE_FSR_INVERT 0
#endif
#ifndef MIXSENSE_GR_DIAGNOSTIC
#define MIXSENSE_GR_DIAGNOSTIC 0
#endif
#if MIXSENSE_I2C_CHECK || MIXSENSE_ENABLE_ERM
#include <Wire.h>
#endif
#if MIXSENSE_ENABLE_ERM
#if MIXSENSE_ADAFRUIT_ERM
#include <Adafruit_DRV2605.h>
#include "AdafruitErmOutput.h"
#else
#include "ErmDriver.h"
#endif
#endif

namespace
{
constexpr int fsrPin = 5, drvSdaPin = 9, drvSclPin = 8;
// Replace with measurements from fsr_monitor before evaluating sensitivity.
constexpr float releasedAdc = 0.0f, pressedAdc = 3000.0f;
constexpr uint32_t sampleMs = 5, settleMs = 500, reportMs = 20;
mixsense::bench::FsrInput fsr(releasedAdc, pressedAdc);
mixsense::bench::HapticOutput haptics;
mixsense::UsbStreamParser parser(mixsense::FrameDirection::HostToDevice);
std::optional<mixsense::UsbFrame> pending;
size_t txOffset = 0;
bool connected = false, initializedSession = false;
uint32_t sessionStarted = 0, lastSample = 0, lastReport = 0, lastLog = 0;
uint32_t receivedHaptics = 0;
#if MIXSENSE_GR_DIAGNOSTIC
uint32_t lastDiagnostic = 0;
bool diagnosticRegisterPhase = false;
#endif

#if MIXSENSE_I2C_CHECK || MIXSENSE_ENABLE_ERM
bool i2cReady = false;
struct DriverRegisters
{
    static constexpr uint8_t address = 0x5a;
    bool read(uint8_t reg, uint8_t& value)
    {
        if (!i2cReady) return false;
        Wire.beginTransmission(address); Wire.write(reg);
        if (Wire.endTransmission(false) != 0
            || Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) return false;
        value = static_cast<uint8_t>(Wire.read());
        return true;
    }
    bool write(uint8_t reg, uint8_t value)
    {
        if (!i2cReady) return false;
        Wire.beginTransmission(address); Wire.write(reg); Wire.write(value);
        return Wire.endTransmission() == 0;
    }
} driverRegisters;
#endif

#if MIXSENSE_ENABLE_ERM
#if MIXSENSE_ADAFRUIT_ERM
Adafruit_DRV2605 adafruitDriver;
mixsense::bench::AdafruitErmOutput<Adafruit_DRV2605, DriverRegisters> motor(adafruitDriver, driverRegisters);
#else
mixsense::bench::ErmDriver<DriverRegisters> motor(driverRegisters);
#endif
#endif

void serviceMotor(uint32_t now)
{
    const float level = haptics.level(now);
#if MIXSENSE_ENABLE_ERM
    motor.service(level, now);
    if (!motor.ready()) haptics.reset();
#else
    (void)level;
#endif
}

#if MIXSENSE_I2C_CHECK
uint32_t lastI2cCheck = 0;
void identifyDriver()
{
    uint8_t status = 0;
    if (!driverRegisters.read(0x00, status))
    {
        Serial.println("0x5A STATUS read FAILED; check power/GND/SDA=9/SCL=8/EN/pullups");
        return;
    }
    const unsigned id = (status >> 5) & 7u;
    Serial.printf("I2C 0x5A ACK status=0x%02X device_id=%u %s; motor=DISABLED\n",
        static_cast<unsigned>(status), id, id == 7 ? "DRV2605L" : "UNEXPECTED DEVICE");
}
#endif

bool hostOpen()
{
    // Arduino 2.0.16 Serial bool requires RTS too; MixSense uses DTR only.
    return tud_mounted() && tud_cdc_connected();
}

bool receiveMessage(const mixsense::TransportMessage& message)
{
    if (haptics.accept(message, millis())) ++receivedHaptics;
    return true;
}

void queueInput(mixsense::MessageKind kind)
{
    const float value = MIXSENSE_FSR_INVERT ? 1.0f - fsr.normalized() : fsr.normalized();
    pending = mixsense::UsbFrameCodec::encode(
        {kind, static_cast<uint16_t>(mixsense::ControlId::Threshold), value},
        mixsense::FrameDirection::DeviceToHost);
    txOffset = 0;
}

void resetSession(uint32_t now)
{
    parser.reset(); pending.reset(); txOffset = 0; haptics.reset();
    serviceMotor(now);
    initializedSession = false; sessionStarted = now; fsr.baseline();
}

#if MIXSENSE_MOTOR_TEST
void motorTest(uint32_t now)
{
    // One explicit key starts one bounded pulse. No automatic boot vibration.
    for (int count = 0; count < 16 && Serial.available(); ++count)
    {
        const char key = static_cast<char>(Serial.read());
        if (key == '0') haptics.reset(); // Test stop cancels pulse too.
        else if (key >= '1' && key <= '3' && motor.ready())
        {
            const float intensity = key == '1' ? 0.5f : key == '2' ? 0.75f : 1.0f;
            haptics.accept({mixsense::MessageKind::Pulse, 0, intensity,
                mixsense::FeedbackTarget::Haptic, 500}, now);
        }
    }
    if (now - lastLog >= 500)
    {
        lastLog = now;
        char line[160];
        const int size = snprintf(line, sizeof(line),
            "ERM driver=%s output=%.2f; keys 1/2/3=500ms at 50/75/100%%; 0=STOP\r\n",
            motor.ready() ? "READY" : "FAULT (reset after fixing wiring)",
            static_cast<double>(haptics.level(now)));
        // A serial monitor that stops reading must not stall pulse expiry.
        if (size > 0 && size < static_cast<int>(sizeof(line)) && Serial.availableForWrite() >= size)
            Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(size));
    }
}
#endif
}

void setup()
{
    pinMode(fsrPin, INPUT);
    analogReadResolution(12); analogSetPinAttenuation(fsrPin, ADC_11db);
    Serial.begin(115200); // Native USB; binary profiles never print here.
    Serial.setTxTimeoutMs(0);
    Serial0.begin(115200); // Optional independent UART debug output.
#if MIXSENSE_ADAFRUIT_ERM
    delay(1500); // Same startup delay as the physically verified Adafruit test.
#endif
#if MIXSENSE_I2C_CHECK || MIXSENSE_ENABLE_ERM
    i2cReady = Wire.begin(drvSdaPin, drvSclPin, 100000); // SDA first, then SCL.
    Wire.setTimeOut(20);
#endif
#if MIXSENSE_ENABLE_ERM
    Serial0.println(motor.begin() ? "3V ERM RTP ready, zero output" : "ERM driver FAULT; output disabled");
#endif
}

void loop()
{
    const uint32_t now = millis();
    if (now - lastSample >= sampleMs)
    {
        lastSample = now;
        fsr.sample(static_cast<float>(analogRead(fsrPin)));
    }
    const bool open = hostOpen();
    if (open != connected)
    {
        connected = open; resetSession(now);
        if (!open) while (Serial.available()) Serial.read();
    }
    serviceMotor(now);

#if MIXSENSE_MOTOR_TEST
    if (open) motorTest(now);
#elif MIXSENSE_FSR_MONITOR
#if MIXSENSE_I2C_CHECK
    if (open && now - lastI2cCheck >= 1000) { lastI2cCheck = now; identifyDriver(); }
#endif
    if (open && now - lastLog >= 100)
    {
        lastLog = now;
        Serial.printf("adc=%.0f normalized=%.3f%s\n",
            static_cast<double>(fsr.filteredRaw()), static_cast<double>(fsr.normalized()),
            fsr.filteredRaw() >= 4080.0f ? " ADC SATURATED" : "");
    }
#else
    if (open)
    {
        for (size_t count = 0; count < 64 && Serial.available(); ++count)
        {
            const auto byte = static_cast<uint8_t>(Serial.read());
            parser.feed(&byte, 1, receiveMessage);
        }
        if (!initializedSession && now - sessionStarted >= settleMs)
        {
            fsr.baseline(); queueInput(mixsense::MessageKind::Snapshot); initializedSession = true;
        }
        if (initializedSession && !pending && now - lastReport >= reportMs && fsr.moved())
        {
            queueInput(mixsense::MessageKind::Value); fsr.baseline(); lastReport = now;
        }
        const int capacity = Serial.availableForWrite();
        if (pending && capacity > 0)
        {
            const size_t count = std::min(pending->size - txOffset, static_cast<size_t>(capacity));
            txOffset += Serial.write(pending->bytes.data() + txOffset, count);
            if (txOffset == pending->size) pending.reset();
        }
    }
    if (now - lastLog >= 1000)
    {
        lastLog = now;
        Serial0.printf("adc=%.0f usb_open=%d rx_haptic=%lu requested=%.3f motor=%s\n",
            static_cast<double>(fsr.filteredRaw()), connected,
            static_cast<unsigned long>(receivedHaptics), static_cast<double>(haptics.level(now)),
#if MIXSENSE_ENABLE_ERM
            motor.ready() ? "ERM READY" : "FAULT"
#else
            "DISABLED"
#endif
        );
    }
#endif
    serviceMotor(millis()); // Apply requests and expire pulses after this iteration's work.
#if MIXSENSE_GR_DIAGNOSTIC
    if (open && now - lastDiagnostic >= 500)
    {
        lastDiagnostic = now;
        uint8_t status = 0, mode = 0, feedback = 0, control3 = 0, rtp = 0;
        const unsigned reads = (driverRegisters.read(0x00, status) ? 1u : 0u)
            | (driverRegisters.read(0x01, mode) ? 2u : 0u)
            | (driverRegisters.read(0x1a, feedback) ? 4u : 0u)
            | (driverRegisters.read(0x1d, control3) ? 8u : 0u)
            | (driverRegisters.read(0x02, rtp) ? 16u : 0u);
        // The native USB endpoint may offer only 64 bytes of TX space.
        char line[64];
        const int length = diagnosticRegisterPhase
            ? snprintf(line, sizeof(line), "REG rd=%02X s=%02X m=%02X f=%02X c=%02X r=%u b=%d a=%.0f\n",
                reads, status, mode, feedback, control3, static_cast<unsigned>(rtp),
                i2cReady, static_cast<double>(fsr.filteredRaw()))
            : snprintf(line, sizeof(line), "DBG ok=%d rx=%lu fault=%s\n", motor.ready(),
                static_cast<unsigned long>(receivedHaptics), motor.faultReason());
        // Diagnostic profile only. Binary-only production profiles never print here.
        if (!pending && length > 0 && length < static_cast<int>(sizeof(line)) && Serial.availableForWrite() >= length)
        {
            Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(length));
            diagnosticRegisterPhase = !diagnosticRegisterPhase;
        }
    }
#endif
    delay(1);
}
