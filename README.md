# ECE496 Capstone

Project documents for the ECE496 capstone.

## Documents

- [MixSense Latency Estimation](docs/MixSense_Latency_Estimation.pdf)

## MixSense prototype (`ec_dev`)

This branch includes the ESP32-S3 FSR/compressor/ERM demo and its development notes.
The original capstone documents above are retained.
The import is based on the tested MixSense team-review snapshot `68b3821`,
copied on 2026-10-28 with its source comments and development records retained.

- [MixSense project overview](README_MixSense.md)
- [October 25 development and test log](docs/20261025.md)
- [Gain-reduction demo](docs/gain-reduction-demo.md)
- [ESP32 FSR/ERM wiring and combined test](firmware/esp32s3/GR_ERM.md)
- [Build instructions](docs/building.md)

The working firmware profile is `mixsense_gr_erm`. FSR pressure controls threshold;
measured compressor gain reduction controls ERM vibration. The earlier physical
standalone test confirmed changing vibration with FSR and stopping with Bypass.
Pressure and vibration calibration, real program audio and DAW tests remain work
in progress.

## Getting started

```sh
git clone --branch ec_dev --recurse-submodules https://github.com/Echo078/ece496-capstone.git
cd ece496-capstone
```

JUCE remains an external submodule at the tested team-review pin. Build output,
PlatformIO caches and nested working copies are excluded from this branch.
