# plugin

The source code for the MixSense audio plugin.

The initial controller bridge, APVTS integration, real-time boundary, and test
workflow are described in [controller architecture](../docs/controller-architecture.md).

## How the plugin builds

The root [`CMakeLists.txt`](../CMakeLists.txt) defines the build.

## Adding a source file

If you add a source file to this directory and you'd like it to be included in the build, make sure
to do the following:

1. Make the `.h` and `.cpp` files in `Source/`.
2. Add the `.cpp` file to `target_sources` in `CMakeLists.txt`:
   ```cmake
   target_sources(MixSense
       PRIVATE
           plugin/Source/ControllerLink.cpp
           plugin/Source/PluginEditor.cpp
           plugin/Source/PluginProcessor.cpp)
   ```
3. Build.

## Include JUCE

If you need to use a JUCE module in your code, include it at the top of the code file:

```cpp
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
```

## Include protocol headers

If you need to use a module from `protocol/` in your code, include a protocol header by its file
name, for example `#include "ControllerEvent.h"`.

## Code that runs on the audio thread

The host calls `processBlock` on its real-time audio thread. Each call has to finish before the host
needs the next block. For example, a block of 512 samples at 48 kHz gives 10.7 ms. If a call takes
longer, the audio drops out.

Because of that, refrain from doing the following in `processBlock`, or in a function that it calls:

- Allocating or releasing memory, for example with `new`, `delete`, or a `std::vector` that grows.
- Locking a mutex, or waiting for a different thread.
- Reading or writing a file, the console, or the USB connection to the controller.
