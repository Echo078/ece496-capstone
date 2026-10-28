# firmware

The firmware for the ESP32-S3 microcontroller in the MixSense controller.

An initial PlatformIO FSR402/USB bring-up project is in [`esp32s3/`](esp32s3/README.md).
It tests GPIO5 FSR402 input, DRV2605L identification (SDA GPIO9 / SCL GPIO8), and
the shared USB protocol. The default image is a text-only sensor monitor.
Explicit `erm_test` and `mixsense_erm` environments add bounded tests and host
control for the user's listed 3V, 12000rpm ERM motor. This is not an LRA configuration.
See the project README for wiring, firmware selection, safeguards and bench limits.

The currently hardware-verified vibration baseline is
[`adafruit_erm`](esp32s3/ADAFRUIT_ERM.md), based on the user's working Adafruit sketch.
The user confirmed increasing vibration through the level sweep on 2026-10-25.

The combined [`mixsense_gr_erm`](esp32s3/GR_ERM.md) profile adds FSR/protocol input
to that Adafruit initialization. The host's explicit GR-haptics switch maps actual
compressor reduction to bounded ERM drive; see the linked procedure for live checks.
