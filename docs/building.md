# Building and running the plugin

CMake reads `CMakeLists.txt` and generates the project files for a native build system. The native
build system then compiles JUCE and the plugin.

---

## Prerequisites

You need CMake 3.22 or newer, a C++17 compiler, and the initialized JUCE submodule.
See the [repository README](../README.md#getting-started) for cloning instructions.

### Windows

Install CMake and the Visual Studio C++ build tools from PowerShell:

```powershell
winget install Kitware.CMake
winget install Microsoft.VisualStudio.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

### macOS

Install Homebrew from [brew.sh](https://brew.sh), follow its "Next steps" instructions,
then install CMake:

```sh
brew install cmake
```

Install Xcode from the App Store. Open it once, accept the license, and let it
install its components. Check the selected developer directory:

```sh
xcode-select -p
```

For the Xcode generator used below, this should point to your Xcode installation,
normally `/Applications/Xcode.app/Contents/Developer`. If it points to
`/Library/Developer/CommandLineTools`, select Xcode:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

## Building

To build with default settings (`Debug` configuration, all targets) from the command line, run the
following commands with the remote repo as your working directory. Note that the first time you
build, it may take a few minutes, because JUCE compiles its helper tool `juceaide`.

#### Windows (PowerShell):

```powershell
cmake -B build
cmake --build build
```

#### macOS (Terminal):

```sh
cmake -B build -G Xcode
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

With only the Xcode Command Line Tools installed, use a Makefiles build instead:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target MixSense_Standalone --parallel 4
open build/MixSense_artefacts/Debug/Standalone/MixSense.app
```

Do not change generators inside an existing build tree; use a different build
directory if it was already configured with Xcode or another generator.

Use a separate build tree on each OS. CMake selects Win32 COM access on Windows
and the POSIX termios backend on macOS; AU is requested only on macOS. The Debug
AU, VST3 and Standalone builds and the original eight hardware-free CTest cases
passed on Apple Silicon for the serial portability baseline, including four
native serial pseudoterminal cases. The later compressor/GR-meter update has been
built as Standalone with all nine current tests passing. Physical FSR-to-UI and
separate ERM-sweep checks have also succeeded. The combined standalone test-tone
test passed with FSR-controlled GR, changing ERM vibration and Bypass stopping the
motor. Real program audio and DAW integration remain unverified.
See [platform support](platform-support.md) for the dependency audit and bench plan.

### Build targets

If you'd like to limit the build to a specific target or set of targets, include
`--target <TARGET_NAME>` at the end of the `cmake --build build` command. The table below shows the
possible targets.

| Target name | Builds |
|---|---|
| `MixSense_Standalone` | The Standalone app (good for quick testing) |
| `MixSense_VST3` | The VST3 plugin |
| `MixSense_AU` | The AU plugin (macOS only) |
| `MixSense_All` | All formats |

Without the `--target` flag, the build compiles all targets.

### Debug and Release

As mentioned, the default configuration is `Debug`. To build `Release`, add `--config Release` to
the `cmake --build build` command.

---

## Running

The build for each target goes into `build/MixSense_artefacts/<configuration>/`, where
`<configuration>` is `Debug` or `Release`:

| Format | Windows | macOS |
|---|---|---|
| Standalone | `Standalone\MixSense.exe` | `Standalone/MixSense.app` |
| VST3 | `VST3\MixSense.vst3` | `VST3/MixSense.vst3` |
| AU | (none) | `AU/MixSense.component` |

### Standalone

You can run the `Standalone` exe by clicking on it or calling it from the command line.
On macOS, open the generated `Standalone/MixSense.app` bundle instead.

The current editor includes a live GR meter and an optional test tone. Follow the
[gain-reduction demo](gain-reduction-demo.md) to check it without an external input.

### VST3 and AU

Put the `.vst3` or `.component` file in the appropriate directory so that your DAW can detect it.
(WIP)
