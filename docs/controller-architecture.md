# Controller architecture

The processor contains a prototype compressor and owns/services the controller
bridge independently of the editor. It can connect a selected Windows COM port or
macOS `/dev/cu.*` USB CDC device. The editor shows actual applied gain reduction
and offers an optional test tone. Its separate development panel submits synthetic
haptic levels. An explicit GR-haptics switch selects measured reduction instead,
with a bounded first-bench mapping and separate host/MCU expiry.

See the [gain-reduction demo](gain-reduction-demo.md) for DSP behavior and a quick test.

See [USB protocol v1](usb-protocol-v1.md) for the shared firmware contract, fully
worked frames, safety/watchdog assumptions, and physical-device testing instructions.

## Components

| Location | Responsibility |
|---|---|
| `protocol/ControlIds.h` | Stable control and feedback target identities; no hardware assignments. |
| `protocol/ControllerTypes.h` | Normalized continuous state, discrete actions, feedback addresses and typed level/pulse payloads. |
| `protocol/UsbProtocolV1.h` | Portable C++17 firmware constants and explicit little-endian helpers, no JUCE. |
| `protocol/GrHapticMapping.h` | Provisional GR deadband/range and signed RTP cap shared by host and combined firmware. |
| `plugin/Source/controller/ControllerTransport.h` | Logical envelope, transport interface and original in-memory mock. |
| `plugin/Source/controller/ControllerBridge.*` | Domain validation, ordered events, coalescing, bounded pending output and configurable rate limiting. |
| `plugin/Source/controller/UsbFrameCodec.*` | Versioned byte encoding and fixed-size incremental parser. |
| `plugin/Source/controller/SerialControllerTransport.*` | Asynchronous device lifecycle, bounded worker exchanges and connection/error state. |
| `plugin/Source/controller/NativeSerialByteStream.cpp` | Existing Win32 serial backend, selected only on Windows. |
| `plugin/Source/controller/MacSerialByteStream.*` | macOS POSIX termios backend, selected by CMake; no Apple SDK/OS types leak into middleware interfaces. |
| `plugin/Source/controller/UnsupportedSerialByteStream.cpp` | Unavailable-device factory for other platforms; mocks remain usable. |
| `plugin/Source/ControllerParameterAdapter.*` | Central parameter definitions and logical-control mapping to APVTS. |
| `plugin/Source/ContinuousFeedbackController.*` | Application-side latest haptic state, generation-based source lease, configurable refresh, and retry of stop-zero requests. |
| `plugin/Source/dsp/DemoCompressor.h` | Prototype stereo-linked compressor; reports the gain reduction actually applied, before makeup. |
| `plugin/Source/dsp/AudioMeterState.h` | Coherent, bounded atomic publication of GR, input peak and producer generation. |
| `plugin/Source/PluginProcessor.*` | Owns DSP, meters, APVTS, adapter, transport and bridge; timer servicing and plugin state persistence. |
| `plugin/Source/PluginEditor.*` | Threshold/bypass attachments, live GR meter, test tone, port status and synthetic feedback controls. |
| `tests/CompressorTests.cpp` | Applied audio attenuation, stereo linking, bypass, makeup, release, meter coherence and test tone. |
| `tests/ControllerTests.cpp` | Original logical vertical slice and bridge/parameter safeguards. |
| `tests/UsbControllerTests.cpp` | Protocol, real serial worker with simulated byte backend, queue/failure/lifecycle and plugin timer tests. |
| `tests/ControllerRefinementTests.cpp` | Deterministic takeover, persistent feedback/source expiry, retry, pulse separation and background-teardown regressions. |

`MixSenseController` remains a JUCE-independent C++17 library.
`TransportMessage` is an internal envelope, never a packed wire structure.
`LogicalMessageCodec` converts the envelope to/from domain types; `UsbFrameCodec`
converts the envelope to/from CDC bytes. APVTS knows nothing about framing.

## Ownership, servicing and real-time boundary

The bridge, mock and parameter adapter belong to JUCE's message thread, including
the synchronous event callback. The processor owns its transport before its bridge,
so the transport outlives the bridge. A processor JUCE timer services the bridge
every 10 ms, including when the editor is closed. This is prototype scheduling,
not a perceptual feedback-rate contract. Feedback interval defaults to no limit
and can be configured separately through `setControllerFeedbackInterval()`.

The serial transport owns one worker, started on construction and idle until an
open request. Only that worker opens/configures/closes the device, reads/writes
bytes, and runs the parser. `connect()` returning true means the asynchronous open
request was accepted; state becomes Connecting, then Connected or Error.
`disconnect()` immediately invalidates the session/queues and requests worker
closure without joining. Reconnection is manual, with a fresh parser and queues.

Transport `send()` and `receive()` use try-locks around bounded memory-only queues,
returning false/idle on contention. No mutex is held during device I/O.
Configuration/lifecycle uses a short memory-only mutex. Pending incoming continuous
state coalesces by control ID; discrete input retains FIFO ordering. Outgoing
levels coalesce by target/address; pulses retain FIFO order. A frame already taken
by the worker is in flight and cannot be replaced. Partial writes continue within
the same session; a write failure ends the session instead of replaying a possibly
partially executed pulse.

| Storage | Bound | Full behavior |
|---|---|---|
| Parser candidate | 12 bytes | Invalid header shifts/rescans; never allocate by advertised length |
| Worker read chunk | 256 bytes | Incremental parser consumes it |
| Transport input | 128 messages | Replace same-control pending state; otherwise drop incoming continuous state with counter; discrete overflow ends/reports session |
| Transport output | 128 messages | Replace same-address pending level; otherwise return false |
| Bridge pending levels | 128 addresses | Replace existing level; reject new address when full |
| Bridge pending pulses | 128 commands | Return false; never overwrite a pulse |

On discrete receive overflow, queued session data is cancelled and ReceiveOverflow
is exposed to the UI. Delivery continuity was lost; events are never silently
treated as replaceable state. Applications must handle false submission results.
Local acceptance does not mean hardware delivery or actuator execution. Transport
counters expose malformed frame candidates and dropped continuous inputs.

Win32 reads return immediately; writes use a finite 50 ms timeout. An idle worker
waits up to 2 ms or wakes on output/lifecycle work. These are I/O/scheduling choices,
not haptic requirements. Port open/configuration/close still depends on the driver,
and all such work stays on the worker.

At processor destruction the service timer stops and detaches its owner callback
under a memory-only mutex, waiting only for an already running service callback.
If teardown is off the dispatch thread, timer-object deletion is posted there;
the stopped timer stays alive until that message is processed/discarded. Its
callback no longer references processor/APVTS state. No MessageManagerLock or
message dispatch is needed to complete processor destruction, avoiding the host
UI-waits-for-background-destructor deadlock. The worker never calls JUCE/UI code.
The transport destructor joins its worker and can wait for the current operation/driver.
Never construct or destroy processors/transports in the audio callback. Byte backends
must provide prompt reads and finite writes; blocked test gates are explicitly
released before destruction.

No transport calls, locks, discovery, parsing or feedback submission run in
`processBlock()`. Audio remains independent of device availability. The compressor
reads cached APVTS raw atomics and publishes one coherent meter snapshot per block
through `AudioMeterState`. Readers make at most two attempts and may skip a busy
publication. Only audio publications advance its lifetime generation; the UI clears
its display after 250 ms without a new publication. Prepare/release invalidate meters.
The serial-device choice is development state, not preset state. Each plugin instance opens
a port exclusively; other serial monitors or plugin instances cannot share it.

## Logical behavior and parameter assumptions

Finite continuous input is clamped to [0, 1]; unknown controls, message kinds and
nonfinite values are rejected. Service has a receive budget of 64 by default.
The original in-memory transport delivers all input in order; serial input may
replace pending same-control state. Discrete presses/releases/increments/decrements
remain individual actions.

### Movement-triggered takeover

`InitialControlState` (wire CONTROL_SNAPSHOT, type 06) is informational, always
ignored for parameter editing. `ContinuousValue` (wire CONTROL_VALUE, type 01) is
genuine calibrated movement and edits APVTS immediately, without matching the DAW
value. The adapter tracks takeover independently for each control. A bridge
session-change callback resets these flags without touching APVTS. Snapshots stay
informational even after movement, UI/automation changes or preset restoration.
Flags are session diagnostics, not permission for snapshots to edit parameters.

There is no first-message heuristic: late/repeated/dropped snapshots do not affect
takeover. Firmware must distinguish stationary publication from user movement;
filtering/threshold/hysteresis are firmware calibration responsibilities. Old
firmware sending startup CONTROL_VALUE must be updated (same wire type cannot
be recognized as both startup and movement).

Transport `sessionId()` tokens expose changes between service calls. The bridge
synchronizes before submission and servicing, clears pending session queues, and
advances its application epoch on token/connection transitions. Serial and mock
implement this token; transport decorators must forward it. This prevents a short
disconnect/reconnect between ticks from replaying old pending commands or preserving
takeover. Session-local snapshots and movements coalesce separately in serial input;
a delayed snapshot cannot replace a pending movement.

Bypass toggles on press and ignores release. Encoder steps use the existing
provisional 0.01 normalized step; bypass increment/decrement explicitly selects
on/off. Each parameter edit uses JUCE begin/update/end gesture and host notification.
Physical gesture timing is not implemented. UI, automation, controller edits and
saved state all use APVTS as the single parameter state.

| Control | Provisional range | Default |
|---|---|---|
| Threshold | -60 to 0 dB | -24 dB |
| Ratio | 1 to 20 :1 | 4 :1 |
| Attack | 0.1 to 200 ms | 10 ms |
| Release | 10 to 2000 ms | 100 ms |
| Makeup gain | -24 to 24 dB | 0 dB |
| Bypass | off/on | off |

Attack/release use skewed ranges. Conversion/snapping is application/JUCE work.
All six parameters remain host-automatable; the custom editor exposes threshold
and bypass. Bypass also uses JUCE's host bypass hook. All six parameters now drive
the prototype compressor; bypass returns dry audio and reports zero gain reduction.

Feedback levels coalesce per address; pulses remain FIFO. No ordering is promised
between pulses and state: the bridge services pulses first. Only state batches
are rate-limited. Haptic zero bypasses that limit and is serviced before pulses so
stale-source stopping is not delayed by ordinary level throttling. Finite feedback is clamped; nonfinite values, unknown target kinds
and zero-duration pulses are rejected. Serial v1 additionally restricts endpoints
to 0 and disallows visual pulses via a transport capability check. The generic mock
retains its broader logical API.

Failed local sends retain bridge output for retry; new levels may replace old ones.
Transport/device failure cancels the session and pending input/output. Old pulses
do not replay on reconnect. An OS write already in flight cannot be retracted.
Opening, closing or losing the controller itself never resets APVTS.
See the protocol document for firmware watchdog/host-refresh responsibilities.

## Build and test without hardware

```powershell
cmake -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`BUILD_TESTING=OFF` disables test executables. To see milestone output:

```powershell
ctest --test-dir build -C Debug -V
```

The original `fakeKnobUpdatesThresholdTest()` and
`fakeGainReductionFeedbackReachesMockTest()` in `ControllerTests.cpp` demonstrate:

```text
[milestone 1] Fake knob 0.25 -> middleware -> JUCE threshold: -24 dB -> -45 dB
[milestone 2] Fake gain-reduction cue 0.6 -> middleware -> mock haptic endpoint 0: level 0.6
```

The `cdc_protocol_transport_integration` CTest uses `SerialControllerTransport`
itself with `SimulatedByteStream` replacing only Win32 operations. It covers split
reads, partial writes, malformed headers, bounded queues, discrete ordering,
coalescing, failed I/O, reconnect, cleanup, and owner calls while worker I/O is held.
The plugin test pumps JUCE timers on the message thread while a separate audio
thread processes throughout traffic, loss and reconnect. No direct adapter or
bridge calls move those serial inputs into APVTS. It also checks unavailable-port
behavior. These tests verify samples/thread separation, not DAW timing, physical
USB behavior, or actuator calibration. Hardware testing is separate; no attached
ESP32-S3 is assumed.

## DSP teammate integration

Keep metrics and musical/haptic mapping in the application layer. After a non-audio
application mapper calculates normalized intensity, use:

```cpp
// JUCE message thread, never the DSP callback:
const bool queued = processor.submitControllerFeedback (
    { { mixsense::FeedbackTarget::Haptic, mixsense::usbv1::hapticEndpoint },
      mixsense::FeedbackLevel { alreadyMappedNormalizedIntensity } });
// false = disconnected, unsupported or bridge full; decide retry policy here.
```

Use `FeedbackPulse { intensity, durationMs }` for timed output.
`setControllerFeedbackInterval()` configures continuous batch spacing. Neither API
claims delivery acknowledgement. Pulses remain discrete; firmware must implement
override/resume using an independent latest continuous baseline, so a refresh does
not truncate a pulse. Multiple-pulse MCU scheduling is still a team decision;
see the firmware test plan in the protocol document.

For persistent mapped feedback, first configure application settings, then use:

```cpp
// Message-thread application setup; values must be selected by the application.
processor.configureContinuousFeedback ({ selectedRefreshInterval, selectedSourceMaxAge });
// Only a NEW underlying source publication advances sourceGeneration:
processor.publishMappedHapticFeedback (mappedNormalizedIntensity, metricValid, sourceGeneration);
// Explicit deactivation, including a pending zero request:
processor.clearContinuousFeedback();
```

`ContinuousFeedbackController` holds one desired level and optional pending zero,
not a command history. It submits changes promptly and repeats unchanged levels
at configured refresh intervals while source data is fresh. A duplicate/older
generation never renews the source lease. Invalid/nonfinite data, zero publication,
explicit clearing or source expiry deactivate nonzero refresh and request zero;
failed submissions are retried without accumulating commands. A zero replaces a
pending nonzero at the bridge/transport coalescing boundaries. Already in-flight
bytes cannot be withdrawn. Accepted submissions still are local queue acceptance.

The class has no production defaults: both refresh interval and source max age
must be positive and configured. Refresh cadence must also account for any bridge
rate limit, UI scheduling stalls, USB delays and the separately agreed MCU watchdog.
Transport freshness is not metric freshness: an open port and repeated cached
metric observations do not prove audio is running. Source expiry stops that old
nonzero state even while communication is healthy. Same-intensity NEW metric
generations renew the lease; repeated observations of one generation do not.

Session changes cancel continuous activity, pending zero and accepted-refresh
timing. The generation watermark survives sessions, so an old cached publication
cannot revive prior-session output. Generations must increase for the lifetime of
the publishing source; resetting that counter needs explicit integration design,
not a reconnect. Publications seen while disconnected are consumed/rejected, not
saved for replay. A fresh post-connect publication can activate output.

The development panel has editable TEST settings (100 ms refresh, 500 ms source
age), Start/update and Stop/zero. Start is an explicit running synthetic source,
independent of audio. It publishes a new synthetic sample on each service tick;
changing intensity updates it, while Stop or closing the panel deactivates it.
A connection-session change disables it; reconnect does not restart vibration.
Those test values are not a production watchdog or perceptual-rate agreement.

The lower-level `submitControllerFeedback()` remains available for one-shot
commands/pulses; use one source owner per continuous endpoint and do not mix raw
level submissions with an active persistent owner. Its latest desired target
will otherwise legitimately win on the next refresh. Pulses may be submitted while
persistent feedback is active, because their state is independent.

`getAudioMeters()` now exposes the latest applied GR, input peak, generation and
audio-running flag through `AudioMeterState`. The message-thread mapper consumes
this snapshot and calls the persistent API only for new publications. Do not
manufacture fresh generations when polling stale data. None of the feedback APIs
is audio-thread safe. The meter does not itself submit haptic commands.
`setGrHapticsEnabled(true)` selects a processor-owned message-thread mapper and
requires a new post-activation audio generation. It uses 50 ms refresh and 250 ms
source expiry, disarms on session changes/editor close, and is mutually exclusive
with the synthetic source. See the GR demo for the shared mapping and the
`mixsense_gr_erm` firmware bench procedure.
