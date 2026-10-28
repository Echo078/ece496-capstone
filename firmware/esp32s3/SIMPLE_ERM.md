# Simple ERM vibration test

For the currently verified hardware baseline, use [ADAFRUIT_ERM.md](ADAFRUIT_ERM.md)
and environment `adafruit_erm`. That version detected the driver and produced an
increasing vibration sweep on 2026-10-25; the custom test below previously failed detection.

Code: `src/simple_erm.cpp`. Build environment: `simple_erm`.
This uses the project's tested `include/ErmDriver.h`; no additional libraries are needed.

With USB/power disconnected, connect:

| DRV2605L / motor | ESP32 / driver |
|---|---|
| VCC | 3.3V directly |
| GND | ESP32 GND |
| SDA | GPIO9 |
| SCL | GPIO8 |
| Motor red | OUT+ |
| Motor black | OUT- |

IN/TRIG is unused; connect to GND if the breakout does not already pull it down.
The user's breakout has no exposed EN header; no external EN connection is assumed.
IN/TRIG is not EN: do not connect IN to 3.3V as an enable workaround.
The motor's black wire goes
to OUT-, not ESP32 GND. The FSR is not used by this sketch.

1. Click **Disconnect** in MixSense, or quit it, to release the USB serial port.
2. Open this directory in PlatformIO.
3. Select **PROJECT TASKS > simple_erm > General > Upload**.
4. If the native USB upload does not connect, hold BOOT, tap RESET, release BOOT,
   select the new port and retry. After upload, press RESET once if necessary.
5. Open **Monitor** at **115200**. Wait for **READY**.
6. Send `1` (with or without a newline) for one second of vibration; `0` stops early.
7. Send `d` for read-only diagnostics: startup register transactions, chip ID/fault
   bits, and a 0x5A address probe with both SDA/SCL assignments. It stops output
   first and restores SDA=9/SCL=8 afterward; it does not start vibration.

On this Mac, double-click `open_erm_monitor.command` to open a Terminal monitor
for the known ESP32 USB port after uploading and resetting the board.

Shell alternative, from the repository root:

```sh
pio device list
pio run -d firmware/esp32s3 -e simple_erm -t upload --upload-port /dev/cu.YOUR_PORT
pio device monitor --port /dev/cu.YOUR_PORT --baud 115200
```

The serial port name can change after upload. The motor stays off at startup.
The existing driver selects ERM open-loop signed RTP, with a nominal average test
full scale of about 2.42V for the listed 3V motor. It checks I2C and latches faults;
fix the cause and reset if **DRIVER NOT READY** appears. Earlier bench runs had a
driver communication failure, so this message must be resolved before vibration.

For I2C NACK troubleshooting, check supply at the board, common ground, soldered
header contacts and SDA/SCL continuity. TI states that EN-low can still ACK an I2C
transaction, so address-only NACK does not specifically identify an EN problem.
See https://www.ti.com/lit/ds/symlink/drv2605l.pdf, section 8.4.1.3.

Software stops output after one second or USB disconnection. As with the existing
driver, loss of I2C while the driver remains powered can prevent a stop write;
disconnect driver power in that case. Mechanical spin-down is not instantaneous.

Uploading this sketch replaces the ESP32's current FSR protocol firmware.
To return to the working FSR/MixSense test, upload `mixsense_usb` again.
