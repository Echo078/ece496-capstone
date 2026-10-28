# Adafruit baseline for the user's working ERM sketch

Environment: `adafruit_erm`; source: `src/adafruit_erm.cpp`.
Libraries: Adafruit DRV2605 1.2.4 / Adafruit BusIO 1.17.4.

Verified on the connected hardware on 2026-10-25: `drv.begin()=TRUE` after a full
power cycle, and the user reported vibration that grew stronger through the sweep.
Use this as the current working vibration baseline. The cause of the earlier custom
test's NACK was not isolated because initialization and power state changed together.

Matches the user's working initialization:

- `Serial.begin(115200); delay(1500);`
- `Wire.begin(9, 8);`
- `drv.begin(); drv.useERM(); drv.setRealtimeValue(0);`
- `drv.setMode(DRV2605_MODE_REALTIME);`
- RTP levels `0, 30, 50, 70, 90, 110, 127`, each 1 second on and 1 second off.

The loop is changed to a single round started by typing `1` in the monitor.
`0` cancels the round. No vibration starts at boot; USB disconnection stops a running round.
The timing is nonblocking so the stop command can be handled during a level.

Power-cycle both boards before this comparison. This uses the stock Adafruit
initialization and does not apply the custom driver's voltage-clamp configuration.
Use the user's listed 3V ERM, not an LRA.

The startup message specifically reports `Adafruit drv.begin()=TRUE/FALSE`.
TRUE means the library's address-detection check passed; actual vibration still
needs verification. The library's setters do not return per-write delivery results.

Pins remain SDA=GPIO9, SCL=GPIO8; motor wires go to OUT+/OUT-.
No external EN header is assumed on this breakout; IN is not EN.

Build/upload from the repo root:

```sh
pio run -d firmware/esp32s3 -e adafruit_erm -t upload --upload-port /dev/cu.YOUR_PORT
```

After power-cycling, double-click `open_erm_monitor.command` (same runtime USB
port and 115200 baud), wait for READY, then type `1` for one round.

This is a comparison baseline, not evidence that the earlier failure was caused
by either wiring or the custom driver. Preserve observed results before concluding.
