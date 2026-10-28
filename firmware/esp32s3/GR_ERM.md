# Combined FSR / measured GR / ERM bench

On 2026-10-25 the user confirmed the combined live test: motor vibration changes
with FSR pressure and Bypass stops the motor. This followed restoration of the
normal firmware and a full USB power cycle. The result is recorded in
`bench-results/2026-10-25-gr-erm-integration.json`. Pressure calibration, perceived
strength calibration, real program audio and DAW testing remain separate checks.

Environment: `mixsense_gr_erm`. Uses the physically verified Adafruit DRV2605
1.2.4 initialization, BusIO 1.17.4, SDA GPIO9, SCL GPIO8, and a 1500 ms startup
delay. ERM output starts at zero. It does not run the sweep automatically.

Keep the existing wiring: FSR measurement on GPIO5, DRV2605L supplied at 3.3 V,
common ground, ERM across OUT+/OUT-. IN is unconnected. FSR divider is
3.3 V -> 2.2 kohm -> FSR -> measurement node -> 10 kohm -> GND.

```sh
pio run -d firmware/esp32s3 -e mixsense_gr_erm
pio run -d firmware/esp32s3 -e mixsense_gr_erm -t upload --upload-port /dev/cu.usbmodem101
```

Close serial monitors and MixSense's CDC connection before upload. Runtime USB
on this board is `/dev/cu.usbmodem348518B5D36C1`; the programming port can be
`/dev/cu.usbmodem101`. After upload on this bench setup, unplug USB for at least
5 seconds and reconnect without BOOT. RESET alone restarts the ESP32 while leaving
the USB-powered driver supplied. In a same-firmware diagnostic comparison, the
driver was undetectable before a full power cycle and ready afterwards; the
underlying reset/power cause is not yet isolated.

In the rebuilt MixSense app:

1. Connect the runtime port using **Connect CDC**.
2. Keep speaker/headphone volume low. Enable **Test tone (-18 dBFS)** and leave
   Bypass off. Leave **Enable GR haptics (ERM)** off initially.
3. Press/release the FSR. More pressure should lower Threshold and increase the
   visible GR. Release should return the threshold near 0 dB and GR toward zero.
4. Enable **Enable GR haptics (ERM)**. Repeat gentle pressure changes. Motor drive
   follows measured GR, rather than being driven directly by FSR pressure.
5. Bypass should request motor off immediately. Disabling the test tone with no
   other input should also stop it. Uncheck GR haptics when finished.

The provisional ADC calibration is still 0..3000, smoothed every 5 ms with a 2%
movement deadband and at most 50 reports/s. This is not force calibration; clamp
and saturation can reduce the useful pressure range. This profile inverts the
normalized pressure for threshold: released -> 0 dB, full mapped pressure -> -60 dB.
Initial CONTROL_SNAPSHOT is informational and does not overwrite the host threshold.

`protocol/GrHapticMapping.h` holds the bench mapping: <=0.5 dB GR is off; above that,
RTP rises from 30 to 90 over 0.5..18 dB GR. Both host and firmware cap at 90/127.
This is a drive-code cap, not a measured motor voltage or calibrated sensation.
The original sweep reached 127; this profile uses a lower maximum for initial tests.
Real audio without GR should produce no vibration, even with the FSR pressed.

Only valid endpoint-0 haptic messages renew the 1000 ms MCU watchdog. USB session
changes clear state. Driver configuration/status is checked every 100 ms and bus
errors latch a fault requiring reset; no stale nonzero state is replayed on recovery.
The Adafruit API initializes the device; checked register transfers apply ongoing
drive and stop requests. If the I2C bus fails while the driver remains powered,
software cannot guarantee delivery of a stop write. This wiring has no MCU-driven EN.

Native USB carries binary protocol only. UART0 provides separate ADC/driver debug
output at 115200 if a separate UART adapter is connected. Protocol v1 has no driver
readiness acknowledgement; “CDC connected” confirms the serial connection.

See the [GR demo and calculation](../../docs/gain-reduction-demo.md) and
[TI Control3 / signed RTP definition](https://www.ti.com/document-viewer/DRV2605L/datasheet/control3-address-0x1d-slos8548529).

## Diagnostic profile

`mixsense_gr_erm_debug` uses the same input, protocol and motor code but adds
bounded `DBG` text to native USB. Use it only with a diagnostic reader; it is not
the binary-only application profile. No automatic vibration is added. Reports
include the latched fault reason, register-read success mask, live STATUS/MODE/
FEEDBACK/CONTROL3/RTP registers and received haptic count. Two alternating reports
stay below the native USB endpoint's 64-byte transmit space. `REG rd=1F` means all
five register reads succeeded. `DBG ok=1 fault=none` means the driver is ready.
Debug text does not acknowledge
physical motor motion. Restore `mixsense_gr_erm` for the normal application test.
