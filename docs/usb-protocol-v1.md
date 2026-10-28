# MixSense USB CDC application protocol v1

Status: implemented host contract, pending firmware-team/physical-device validation.
This document is the hardware/software agreement for the first CDC prototype.
`protocol/ControlIds.h` and `protocol/UsbProtocolV1.h` are portable C++17 headers
with numeric definitions and explicit endian helpers; they do not require JUCE.

## Roles and serial configuration

The ESP32-S3 is the USB CDC-ACM **device**; the computer is the USB **host**.
The host explicitly selects a Windows COM port or macOS `/dev/cu.*` path. No USB VID/PID or
GPIO assignment is part of this protocol. The host opens the port exclusively;
close firmware flashing tools and serial monitors before connecting MixSense.

The Windows backend requests 115200 baud, 8 data bits, no parity, one stop bit,
no software/hardware flow control, DTR asserted and RTS deasserted. CDC firmware
may treat baud as line-coding metadata rather than a physical bit rate. Confirm
line-coding and DTR/reset behaviour with the firmware team. Do not send diagnostic
text over this binary stream; use a separate logging interface.

The macOS backend uses raw termios at the same 115200/8N1 with no flow control,
nonblocking reads and bounded writes. Its driver controls modem-line/reset behavior;
Mac hardware testing is still required. See [platform support](platform-support.md).
This OS backend addition does not change any application-protocol bytes below.

The backend follows Microsoft's [communications resource handle rules](https://learn.microsoft.com/en-us/windows/win32/devio/communications-resource-handles)
and [COMMTIMEOUTS semantics](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts)
for exclusive port access, immediate reads and finite worker write timeouts.

## Frame layout

| Offset | Field | Size | Encoding |
|---|---|---|---|
| 0 | Magic | 2 bytes | `AA 55` |
| 2 | Version | 1 byte | `01` |
| 3 | Type | 1 byte | Table below |
| 4 | ID | 1 byte | Logical control ID or feedback endpoint |
| 5 | Length | 1 byte | Payload bytes only, exact length for type |
| 6 | Payload | 0, 2, or 6 bytes | Table below |

Maximum payload is **6 bytes**; maximum frame is **12 bytes**. There is no padding,
terminator, timestamp, acknowledgement, or checksum. Multi-byte integers are
unsigned and **little-endian**. Frames are constructed byte by byte, never by
copying packed C/C++ structures.

CDC is a byte stream: reads may contain part of a header, part of a payload,
multiple frames, or no data. Both peers need incremental parsing.

## Message types

| Type | Name | Direction | ID | Payload |
|---|---|---|---|---|
| `01` | CONTROL_VALUE | Device → Host | Control | `uint16` normalized value |
| `02` | BUTTON_PRESSED | Device → Host | Control | none (length 0) |
| `03` | BUTTON_RELEASED | Device → Host | Control | none (length 0) |
| `04` | ENCODER_INCREMENT | Device → Host | Control | none (one action) |
| `05` | ENCODER_DECREMENT | Device → Host | Control | none (one action) |
| `06` | CONTROL_SNAPSHOT | Device → Host | Control | `uint16` initial/reported position (length 2), never a parameter edit |
| `10` | HAPTIC_LEVEL | Host → Device | Haptic endpoint | `uint16` intensity (length 2) |
| `11` | HAPTIC_PULSE | Host → Device | Haptic endpoint | `uint16` intensity, then `uint32` duration in milliseconds (length 6) |
| `12` | VISUAL_LEVEL | Host → Device | Visual endpoint | `uint16` level (length 2) |

`CONTROL_VALUE` now explicitly means **genuine physical movement**, not startup
publication. `CONTROL_SNAPSHOT` is the additive initial-state message; version,
header, existing type numbers, IDs and payload sizes are unchanged.

No visual pulse exists in v1. The type identifies the feedback target; there is
no separate target byte. `FeedbackTarget::Haptic = 0` and `Visual = 1` are logical
C++ identities, **not** type bytes to put on the wire.

### Logical control IDs

| ID (decimal/hex) | Control |
|---|---|
| 0 / `00` | Threshold |
| 1 / `01` | Ratio |
| 2 / `02` | Attack |
| 3 / `03` | Release |
| 4 / `04` | MakeupGain |
| 5 / `05` | Bypass |

IDs 6–255 are unsupported in v1. These IDs preserve the initial logical enum
values. No ID describes a potentiometer pin or physical knob location.
The plugin initially uses threshold value changes and bypass button presses;
the other parameters/mappings remain available. A bypass press toggles the JUCE
boolean; release does not toggle it again. Increment/decrement are individual
actions, with the existing provisional application step of 0.01 normalized.

### Feedback endpoint IDs

| Type family | Endpoint ID | Logical destination |
|---|---|---|
| Haptic level/pulse | 0 / `00` | Primary haptic output |
| Visual level | 0 / `00` | Primary visual output |

All other endpoint IDs are rejected in v1. Endpoint 0 does not imply an I2C
address or actuator channel. Firmware owns endpoint lookup, including whether
the prototype physically provides a visual output.

### Values and validation

`0` means normalized 0.0; `65535` means normalized 1.0. The host encodes finite
floats as `round(clamp(value, 0, 1) * 65535)` and decodes as `integer / 65535.0`.
Every uint16 value is valid; there are no transmitted floating-point NaNs.
Haptic intensity is logical normalized drive, not acceleration, gain reduction,
voltage, or a DRV2605L register value. Visual level is similarly normalized.
Pulse duration must be nonzero; it is unsigned 32-bit milliseconds. Firmware may
apply calibrated duration/output limits, and agreement on those limits is still needed.

Reject wrong magic, unsupported version/type/direction/ID, incorrect exact payload
length, payloads above the bound, and zero-duration pulses. Unsupported versions
are discarded; there is no fallback negotiation. Unknown controls never change
JUCE parameters. Outgoing unsupported logical addresses/types are rejected before
bridge acceptance for a serial transport.

The parser scans for `AA 55`, retains at most one 12-byte candidate, checks the
complete header before waiting for payload, and advances by one byte after an
invalid candidate. This handles overlapping magic and recovers after noise or a
malformed header without trusting an unbounded advertised length. Reinitialize
the parser on every session. Valid continuous messages describe replaceable state;
presses/releases/encoder steps and pulses are discrete actions.

### Integrity decision and limits

No application checksum is included in v1 because USB supplies link-level error
detection/retry. This does **not** detect firmware/software bugs, incorrectly
constructed payloads, stale data, or every stream synchronization error. Noise
that happens to form a valid frame can be accepted. A plausible but incomplete
frame can temporarily consume bytes from a subsequent frame before resynchronizing.
Specifically, a CONTROL_VALUE header missing its two payload bytes can accept the
next frame's `AA 55` as value `0x55AA`, losing that next frame and potentially
editing a parameter with a bogus value. A subsequent complete frame resynchronizes.
If bench testing requires rejecting all such candidates, agree a future integrity/
framing revision (for example checksum plus delimiter/escaping, or a carefully
specified assembly timeout). Silently discarding every embedded magic would also
discard legitimate uint16/pulse payloads, so v1 parsing has not been changed that way.
There is no frame assembly timeout or escaping; the small fixed bound limits parser
storage.

## Worked messages

### Device → host: threshold knob at 25%

```text
AA 55 01 01 00 02 00 40
----- -- -- -- -- -----
magic ver type ID len payload
```

`type=01`, threshold `ID=00`, length 2, `00 40` is little-endian `0x4000 = 16384`.
Decoded normalized value is about 0.2500038. With the plugin's provisional
threshold range [-60, 0] dB and snapping, APVTS becomes **-45 dB**.

Bypass press/release are `AA 55 01 02 05 00` / `AA 55 01 03 05 00`.

### Host → device: primary haptic intensity at 50%

```text
AA 55 01 10 00 02 00 80
```

`type=10`, endpoint `00`, length 2, `00 80` is `0x8000 = 32768`, about 0.5000076.
Firmware interprets this through its calibrated actuator limits.

| Requested logical level | uint16 | Payload bytes |
|---|---|---|
| 0.0 | 0 | `00 00` |
| 0.25 | 16384 | `00 40` |
| 0.50 | 32768 | `00 80` |
| 0.75 | 49151 | `FF BF` |
| 1.0 | 65535 | `FF FF` |

A 100 ms pulse at 50% is `AA 55 01 11 00 06 00 80 64 00 00 00`.
A visual level at 50% is `AA 55 01 12 00 02 00 80`.

### Initial state versus movement

An initial threshold position at 25% is **`AA 55 01 06 00 02 00 40`**. This does
not edit APVTS. A subsequent genuine movement to 25% uses the existing
`AA 55 01 01 00 02 00 40`, which edits threshold to -45 dB with current ranges.
No first-message rule or initialization window exists: lost, repeated, or delayed
snapshots never become movements, even after another control has taken over.
Missing snapshots do not prevent an actual movement from being accepted.

Firmware must initialize/filter each calibrated ADC position, publish it as a
snapshot if needed, then emit CONTROL_VALUE only after detecting actual movement.
Choose movement threshold, noise filtering and hysteresis on the hardware side;
periodic stationary reports must use snapshots, not repeated CONTROL_VALUE packets.
Do not label ADC startup settling or noise as user movement. The host cannot infer
physical intent from absolute values alone. Each control is independent; no
physical-position-to-DAW-value matching (pickup) is required.

**Firmware coordination required:** old firmware that publishes startup positions
as CONTROL_VALUE will still cause parameter edits. It must be updated. There is
no handshake/capability flag by which this host can identify that legacy behavior.
Older hosts discard unknown snapshot type 06 and still accept genuine type 01
movement, so the change is additive at the framing level. Clear MCU transmit queues
at a new USB/CDC session; never replay prior-session movements.

## Connection and loss semantics

Opening the selected serial device starts a new local session. There is no handshake or MCU
reset command. Startup bytes may arrive after DTR or firmware boot; parser noise
recovery does not replace proper separation of binary data and logging.
On a new session the MCU should publish its calibrated current control state.
The host retains existing APVTS state until actual movement/discrete input arrives;
initial snapshots are informational. Opening,
closing, or losing the device itself never changes compressor parameters.

Host connection errors stop outgoing acceptance and clear pending session input
and output. Reconnection is manual, with a fresh parser and empty queues; old pulses
are never replayed. A write already in progress/accepted by the OS cannot be
retracted by disconnect. UI "queued locally" means neither acknowledged delivery
nor actuator execution. Partial successful writes are continued within the same
session; on failure the entire session ends instead of resending a possibly
partially executed pulse. Malformed frames are discarded; discrete-event queue
overflow is reported as a session error rather than silently coalescing events.

Without heartbeat/acknowledgements the host detects device loss when Win32 reports
an I/O/port error; it cannot establish that silent firmware is alive or responsive.

## Firmware responsibilities and watchdog proposal

```text
Potentiometer startup/report -> calibration/filtering -> CONTROL_SNAPSHOT
Genuine calibrated potentiometer movement -> normalized uint16 -> CONTROL_VALUE
Button debounce -> one press/release action -> BUTTON_PRESSED / BUTTON_RELEASED
HAPTIC_LEVEL -> endpoint lookup -> calibrated intensity -> DRV2605L RTP command
HAPTIC_PULSE -> timed override -> return to latest continuous baseline
```

This host does not modify firmware. Firmware must store the continuous baseline separately from an
active pulse. A level/refresh received during a pulse updates that baseline
without changing/truncating the pulse. On completion resume the **latest** baseline.
For level 0.4, pulse 0.9/100 ms, level 0.6: output remains 0.9 through the pulse,
then becomes 0.6. A zero level clears the baseline but does not cancel a currently
timed discrete pulse; watchdog/detach/reset cancels both. Host sends duration once,
does not sleep for it, and never periodically refreshes a pulse.

Host pulse-first ordering within a service batch remains valid with this contract:
the subsequent level is a baseline update, not a pulse cancellation. Pulse FIFO
order is preserved on the host, but exact actuator timing is MCU-owned.

**Multiple-pulse scheduling still needs firmware-team agreement.** Recommended
policy if every full pulse must play: FIFO with a bounded MCU queue and observable
overflow policy. Simpler alternative: newest pulse preempts/restarts the active
pulse, reducing state/latency but shortening earlier pulses. The host does not
assume either policy, does not space pulses using sleeps, and does not claim all
durations executed. No new wire message is necessary for either policy.

The MCU must set haptic output to zero on USB detach/reset, and should additionally
use a **configurable command watchdog** refreshed only by a valid haptic command.
If it expires, cancel pulses and set output to zero. Agree the timeout and host
refresh interval after measuring the link; there is intentionally no production
timeout constant in this host code. The application-side continuous feedback
controller now periodically resubmits an unchanged active level at a configurable
interval. Its separate source lease expires if no genuinely new mapped metric
generation arrives: it stops nonzero refresh and requests zero. Transport activity
does not renew that source lease. Settings must be configured by the application;
the development panel's editable 100 ms refresh / 500 ms source-age values are test
settings only, not a watchdog agreement or production/perceptual requirement.

The synthetic panel is an explicitly running fake source while Start is active;
it publishes fresh synthetic samples even without audio. Stop or closing that
panel requests zero. A session change disables it; reconnect requires Start again.
Invalid/visual/control traffic must not keep stale haptic output alive. A UI stall can delay a
zero request, so the independent MCU watchdog remains necessary. Already in-flight
commands cannot be retracted and local acceptance still is not acknowledgement.

## Bench testing

Build using the commands in [controller architecture](controller-architecture.md).
Launch `build/MixSense_artefacts/Debug/Standalone/MixSense.exe`, or load the VST3 in
a DAW. The plugin editor contains the current development controls:

1. Identify the ESP32-S3 CDC COM port in Windows Device Manager. Close other users
   of that port. Enter `COM7` (replace with the actual port) and click **Connect CDC**.
2. Wait for **CDC connected**. Open errors are reported without changing audio.
3. Set threshold in the UI, then have firmware send the snapshot example, including
   repeated snapshots. The threshold must stay unchanged. Send an actual movement
   frame; the 25% example should change it to -45 dB. Repeat after preset restore,
   UI/automation edits, and reconnect. Test controls independently.
4. Send the bypass press/release pair and confirm one bypass toggle.
5. Select 0.5 and positive development refresh/source-age settings, then click
   **Start / update persistent**. Firmware should keep receiving HAPTIC_LEVEL 0.5
   on endpoint 0 without slider movement. Change to 0.75, then click
   **Stop / request zero** and confirm zero. Confirm decoded bytes independently
   of calibrated actuator response. The UI is not hardware receipt confirmation.
6. While audio runs, unplug the board. The host should report an error when the
   driver detects loss, retain parameters, and continue passthrough audio. Verify
   that the firmware watchdog/detach handling zeros the actuator independently.
7. Reconnect the board, recheck its COM assignment, and connect manually. Confirm
   no old pulses or persistent intensity replay. Snapshots must preserve APVTS;
   actual movement must resume editing. Explicitly Start feedback again.

### Firmware pulse/watchdog verification plan (hardware required)

- Set baseline 0.4, send pulse 0.9/100 ms, then send/refresh baseline 0.6 before
  completion. Instrument output/timestamps on the MCU: pulse must not truncate,
  and resume value must be 0.6. Host tests verify the message sequence only.
- Stop continuous output during a pulse: baseline becomes zero and the existing
  pulse finishes. Force watchdog expiry/detach/reset: pulse must cancel immediately
  and output must become zero, with no queued pulse replay after recovery.
- Submit two pulses and validate the separately agreed bounded scheduling policy.
- Stop real metric publication while CDC stays open. Host must stop nonzero
  refresh after configured source age and request zero. Stall host servicing too
  and verify the MCU watchdog independently zeros output after its agreed timeout.
- Reopen CDC with partial frames outstanding and verify MCU parser/transmit buffers
  reset cleanly. Exclusive port ownership is intentional; test a second plugin
  instance reports an open failure without changing audio or the first connection.
