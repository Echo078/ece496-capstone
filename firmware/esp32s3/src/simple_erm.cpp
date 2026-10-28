// Minimal 3V ERM test. PlatformIO environment: simple_erm
// DRV2605L: SDA -> GPIO9, SCL -> GPIO8, VCC -> 3.3V, GND -> GND.
// Motor: red -> OUT+, black -> OUT-. No FSR or MixSense connection needed.
#include <Arduino.h>
#include <Wire.h>
#include <tusb.h>
#include "ErmDriver.h"

namespace
{
constexpr uint8_t driverAddress = 0x5a;
constexpr uint32_t vibrationMs = 1000;
bool busReady = false;

// Small I2C adapter for the already-tested ERM driver.
struct Registers
{
    struct Transaction { char operation; uint8_t reg, value, result; };
    Transaction startup[40] {};
    size_t startupCount = 0;
    bool recordStartup = true;
    void record(char operation, uint8_t reg, uint8_t value, uint8_t result)
    {
        if (recordStartup && startupCount < 40)
            startup[startupCount++] = {operation, reg, value, result};
    }
    bool read(uint8_t reg, uint8_t& value)
    {
        Wire.beginTransmission(driverAddress);
        Wire.write(reg);
        const uint8_t result = Wire.endTransmission(false);
        if (result != 0) { record('R', reg, 0, result); return false; }
        if (Wire.requestFrom(driverAddress, static_cast<uint8_t>(1)) != 1)
        { record('R', reg, 0, 0xfe); return false; }
        value = static_cast<uint8_t>(Wire.read());
        record('R', reg, value, 0);
        return true;
    }
    bool write(uint8_t reg, uint8_t value)
    {
        Wire.beginTransmission(driverAddress);
        Wire.write(reg);
        Wire.write(value);
        const uint8_t result = Wire.endTransmission();
        record('W', reg, value, result);
        return result == 0;
    }
} registers;

mixsense::bench::ErmDriver<Registers> motor(registers);
bool vibrating = false;
uint32_t started = 0, lastStatus = 0;

void probePins(int sda, int scl)
{
    Wire.end();
    const bool initialized = Wire.begin(sda, scl, 100000);
    Wire.setTimeOut(20);
    Serial.printf("I2C probe SDA=%d SCL=%d: init=%d, SDA_level=%d SCL_level=%d\r\n",
        sda, scl, initialized, digitalRead(sda), digitalRead(scl));
    if (!initialized) return;
    Wire.beginTransmission(driverAddress);
    const uint8_t result = Wire.endTransmission();
    Serial.printf("  address 0x5A result=%u (0=ACK, 2=NACK, 5=timeout)\r\n", result);
    if (result == 0)
    {
        uint8_t status = 0;
        if (registers.read(0x00, status))
            Serial.printf("  STATUS=0x%02X device_id=%u (DRV2605L expects 7)\r\n", status, status >> 5);
        else Serial.println("  Address ACK, but repeated-start register read failed");
    }
}

void diagnose()
{
    vibrating = false;
    motor.service(0.0f, millis());
    Serial.printf("\r\nDIAGNOSTIC: Wire.begin=%d, motor.ready=%d\r\n", busReady, motor.ready());
    Serial.println("Startup transactions: result=0 success, 0xFE=register read returned no byte");
    for (size_t i = 0; i < registers.startupCount; ++i)
    {
        const auto& tx = registers.startup[i];
        Serial.printf("  %c reg=0x%02X value=0x%02X result=0x%02X\r\n",
            tx.operation, tx.reg, tx.value, tx.result);
        if (tx.operation == 'R' && tx.result == 0)
        {
            if (tx.reg == 0x00)
                Serial.printf("    device_id=%u, OC/OT fault bits=0x%02X\r\n", tx.value >> 5, tx.value & 3);
            for (size_t j = i; j > 0; --j)
            {
                const auto& previous = registers.startup[j - 1];
                if (previous.operation == 'W' && previous.reg == tx.reg && previous.result == 0)
                {
                    if (previous.value != tx.value)
                        Serial.printf("    READBACK DIFFERENCE: wrote 0x%02X, read 0x%02X\r\n", previous.value, tx.value);
                    break;
                }
            }
        }
    }
    probePins(9, 8);
    probePins(8, 9); // Read-only check for a reversed physical pin assignment.
    Wire.end();
    Wire.begin(9, 8, 100000); Wire.setTimeOut(20);
    Serial.println("Restored SDA=9 SCL=8. Diagnostic did not start vibration.\r\n");
}
}

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    busReady = Wire.begin(9, 8, 100000); // SDA, SCL, frequency.
    Wire.setTimeOut(20);
    if (busReady) motor.begin(); // Initializes ERM mode with zero output.
    registers.recordStartup = false;
}

void loop()
{
    const uint32_t now = millis();
    const bool usbOpen = tud_mounted() && tud_cdc_connected();

    // Start only on an explicit command. Stop on timeout or USB disconnection.
    if (!usbOpen || now - started >= vibrationMs) vibrating = false;
    if (usbOpen)
    {
        for (int count = 0; count < 16 && Serial.available(); ++count)
        {
            const char command = static_cast<char>(Serial.read());
            if (command == 'd' || command == 'D') { diagnose(); continue; }
            if (command == '0') vibrating = false;
            if (command == '1' && motor.ready())
            {
                started = now;
                vibrating = true;
            }
        }
    }

    // 1.0 is the driver's limited test full scale, nominal average ~2.42V.
    motor.service(vibrating ? 1.0f : 0.0f, now);
    if (!motor.ready()) vibrating = false;

    if (usbOpen && now - lastStatus >= 1000)
    {
        lastStatus = now;
        const char* message = motor.ready()
            ? (vibrating ? "VIBRATING (1 second); type 0 to stop\r\n"
                         : "READY: 1=vibrate, 0=stop, d=diagnose\r\n")
            : "DRIVER NOT READY: type d for exact I2C/startup diagnostic\r\n";
        const size_t length = strlen(message);
        // Avoid blocking motor stopping if the serial monitor stops reading.
        if (Serial.availableForWrite() >= static_cast<int>(length))
            Serial.write(reinterpret_cast<const uint8_t*>(message), length);
    }
    delay(1);
}
