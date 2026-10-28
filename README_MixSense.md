# MixSense

MixSense is a JUCE-based audio plugin prototype connected to an ESP32-S3 haptic
controller over USB CDC. See the [controller architecture](docs/controller-architecture.md)
for the current implementation and integration interfaces.

The current prototype also includes a basic compressor, a live gain-reduction
meter, and an optional test tone. See the [gain-reduction demo](docs/gain-reduction-demo.md)
for a quick standalone test and the combined FSR/ERM bench procedure.

## Getting started

Clone the `ec_dev` branch with its JUCE submodule:

```sh
git clone --branch ec_dev --recurse-submodules https://github.com/Echo078/ece496-capstone.git
```

For an existing checkout with an empty `JUCE/` directory, run
`git submodule update --init` from the repository root.

- [Machine setup, building and running](docs/building.md)
- [Platform support, automated tests and hardware verification](docs/platform-support.md)
- [Contributing](docs/contributing.md)
- [USB CDC protocol and hardware bench procedure](docs/usb-protocol-v1.md)

## Repository layout

| Path | Contents |
|---|---|
| [`plugin/`](plugin/) | Audio plugin and host controller middleware |
| [`protocol/`](protocol/) | Control definitions and USB protocol constants shared with firmware |
| [`firmware/`](firmware/) | ESP32-S3 controller firmware |
| [`tests/`](tests/) | Hardware-free compressor, meter, controller, protocol and native serial tests |
| [`docs/`](docs/) | Build, architecture, protocol and contribution documentation |
| `JUCE/` | JUCE framework, included as a Git submodule |
| [`CMakeLists.txt`](CMakeLists.txt) | Build and test configuration |
