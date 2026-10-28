// Based on the user's working 2026-09-07 Adafruit sketch.
// Same library, pins, startup delay, ERM/RTP initialization and test levels.
// Type 1 for ONE round; 0 stops. No vibration at startup.
#include <Adafruit_DRV2605.h>
#include <Arduino.h>
#include <Wire.h>
#include <tusb.h>

Adafruit_DRV2605 drv;
constexpr int I2C_SDA_PIN = 9, I2C_SCL_PIN = 8;
constexpr uint8_t TEST_LEVELS[] = {0, 30, 50, 70, 90, 110, 127};
constexpr uint32_t LEVEL_DURATION_MS = 1000, PAUSE_DURATION_MS = 1000;

bool driverFound = false, running = false, pausePhase = false;
bool previousUsbOpen = false;
size_t levelIndex = 0;
uint32_t phaseStarted = 0, lastReport = 0;

void stopMotor()
{
    if (driverFound) drv.setRealtimeValue(0);
    running = false;
}

void setup()
{
    Serial.begin(115200);
    delay(1500); // Match the user's working startup sequence.
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    driverFound = drv.begin();
    if (!driverFound) return;
    drv.useERM();
    drv.setRealtimeValue(0);
    drv.setMode(DRV2605_MODE_REALTIME);
}

void loop()
{
    const uint32_t now = millis();
    const bool usbOpen = tud_mounted() && tud_cdc_connected();
    if (!usbOpen && previousUsbOpen) stopMotor();
    previousUsbOpen = usbOpen;

    if (usbOpen)
    {
        for (int count = 0; count < 16 && Serial.available(); ++count)
        {
            const char command = static_cast<char>(Serial.read());
            if (command == '0') stopMotor();
            if (command == '1' && driverFound)
            {
                drv.setRealtimeValue(0);
                running = true; pausePhase = false; levelIndex = 0; phaseStarted = now;
            }
        }
    }

    if (running && now - phaseStarted >= (pausePhase ? PAUSE_DURATION_MS : LEVEL_DURATION_MS))
    {
        phaseStarted = now;
        if (!pausePhase) { drv.setRealtimeValue(0); pausePhase = true; }
        else
        {
            pausePhase = false;
            if (++levelIndex == sizeof(TEST_LEVELS)) stopMotor();
            else drv.setRealtimeValue(TEST_LEVELS[levelIndex]);
        }
    }

    // Keep status output bounded so an unread monitor cannot delay stopping.
    if (usbOpen && now - lastReport >= 1000)
    {
        lastReport = now;
        char line[150];
        int size = 0;
        if (!driverFound)
            size = snprintf(line, sizeof(line), "Adafruit drv.begin()=FALSE: device not detected on SDA=9 SCL=8\r\n");
        else if (running)
            size = snprintf(line, sizeof(line), "Adafruit connected: RTP=%u, %s\r\n",
                static_cast<unsigned>(pausePhase ? 0 : TEST_LEVELS[levelIndex]), pausePhase ? "1s OFF pause" : "1s level");
        else
            size = snprintf(line, sizeof(line), "Adafruit drv.begin()=TRUE: READY; 1=one level sweep, 0=stop\r\n");
        if (size > 0 && size < static_cast<int>(sizeof(line)) && Serial.availableForWrite() >= size)
            Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(size));
    }
    delay(1);
}
