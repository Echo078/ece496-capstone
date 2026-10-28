# Windows and macOS support

## Current bench update — 2026-10-25

The prototype compressor/GR-meter change builds as a macOS Debug Standalone app
using JUCE 8.0.12 and the Command Line Tools Makefiles generator. All nine current
CTest cases pass, including the added `compressor_dsp_and_meter` suite. The editor
layout was checked with an offscreen render. This DSP change has not yet been
verified inside a DAW or rebuilt on Windows.

The separate team-review checkout was then built against the unchanged repository
JUCE pin `3fb5cbc0340758596dac39f7c8de47d562b4896e` (JUCE 9.0.4). Debug Standalone,
VST3 and AU builds and all nine CTest cases passed there. The combined PlatformIO
firmware build and firmware host tests with ASan/UBSan also passed. This validation
did not reflash or operate the hardware; the earlier physical result remains a
separate bench record.

Earlier in this bench session, the user confirmed that physical ESP32-S3 FSR
movement updates the standalone threshold slider, and separately confirmed that
the Adafruit ERM level sweep grows stronger. See `firmware/esp32s3/bench-results/`
for those records. A later combined test with `mixsense_gr_erm` and the host's GR
mapper also passed: the user confirmed that vibration changes with FSR and Bypass
stops the motor, using the standalone test tone. It required a full USB power cycle
after upload to restore driver communication. See the combined integration and
driver-recovery records in that directory. Real program audio and DAW integration
remain unverified.

The sections below describe the earlier native-serial portability baseline and its
verification limits. For the added DSP and meter, see the
[gain-reduction demo](gain-reduction-demo.md).

## Support versus verification

| Platform | Native controller backend | Verification in this change |
|---|---|---|
| Windows | Existing Win32 COM implementation, unchanged | Debug VST3/Standalone build and four hardware-free CTest suites passed |
| macOS | POSIX termios backend for explicit `/dev/cu.*` devices | Debug AU/VST3/Standalone build and all eight hardware-free CTest cases passed on Apple Silicon; physical CDC remains unverified |
| Other desktop platforms | Unavailable-device stub | Controller mocks remain usable; no native Linux serial support is claimed |

No physical ESP32-S3/actuator test was performed on either OS for this change.
The USB application protocol, firmware layout, DSP and feedback mapping are unchanged.

## Dependency audit

| Area | OS dependency / result |
|---|---|
| Byte I/O | `NativeSerialByteStream.cpp` remains Windows-only: `HANDLE`, COM names, DCB, COMMTIMEOUTS and Win32 calls. CMake selects it only on Windows. |
| macOS byte I/O | `MacSerialByteStream.*` confines descriptors, termios, fcntl, ioctl and unistd to the Mac backend; uses the system C library, no third-party serial dependency or IOKit discovery. |
| Middleware | Serial worker, session tokens, bounded queues, bridge, codec and domain headers remain standard C++17. Thread ownership and partial-write handling are unchanged. |
| Plugin/application | JUCE handles native UI/audio/host integration. Port entry now preserves case-sensitive paths; only a syntactically valid COM name is canonicalized. APVTS and processBlock do not depend on the serial OS API. |
| Build | CMake selects exactly one factory/backend. VST3 and Standalone build on both target OSes; AU is requested only on Apple platforms. Threads and JUCE recommended flags stay portable. |
| Toolchain | Windows requires the installed MSVC/Windows SDK setup; macOS requires an Apple toolchain/SDK (Xcode for the documented generator). Each OS needs its own build tree. |
| Architecture/deployment | The project does not hardcode an Intel/Apple Silicon architecture or deployment target. Choose these in CMake/toolchain settings and validate against JUCE/SDK/DAW requirements on the Mac. Universal builds/signing/notarization are not validated here. |
| Runtime access | The process needs permission to open the selected serial device. An in-process plugin runs under the DAW's access restrictions; it cannot grant itself extra host permissions. Each port has one ordinary owner. |

Apple documents serial-device access under the
[App Sandbox serial entitlement](https://developer.apple.com/library/archive/documentation/Miscellaneous/Reference/EntitlementKeyReference/Chapters/EnablingAppSandbox.html).
Test access inside the intended DAW as well as Standalone; this change does not
add sandbox entitlements or signing policies. Distribution/DAW plugin loading
also needs a separately validated signing/notarization setup.

## Port selection and macOS I/O

Use `COM7` on Windows (lowercase `com7` still works), or the exact device path,
such as `/dev/cu.usbmodemMiXeD123`, on macOS. The application trims surrounding
whitespace, but does not uppercase POSIX paths. Configuration strings are bounded
to 1024 bytes and reject embedded NULs. The Mac backend accepts one nonempty
`/dev/cu.*` component, rejects nested paths and symlinks, and verifies that the
opened descriptor is a character device with termios support. `/dev/tty.*` and
arbitrary files are intentionally not supported by the production Mac factory.

The backend uses `O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW`,
exclusive TTY ownership (`TIOCEXCL`), raw 115200/8N1, receiver enabled, and no
software or hardware flow control. Settings are saved and restored on close.
Modem-line behavior on macOS is left to the driver; the Windows implementation
still explicitly asserts DTR and deasserts RTS. Confirm ESP32-S3 DTR/reset behavior
on a real Mac; do not assume the two drivers reset the board identically.

Reads never wait for input: EAGAIN/EWOULDBLOCK/EINTR means idle/retry next worker
iteration. With nonblocking VMIN=1/VTIME=0, EOF or another error ends the session.
Writes return partial progress immediately. EAGAIN/EWOULDBLOCK/EINTR retries use
at most a 50 ms steady-clock budget per call, with short worker-only backoffs.
The serial worker continues partial writes within the current session and handles
failure/disconnect exactly as before. No mutex is held during OS I/O. Error
detection is OS/driver based, not firmware execution/readiness confirmation.

Close discards queued kernel I/O, restores settings with TCSANOW, releases
exclusive mode, then closes the descriptor. It does not call tcdrain or wait for
the device to consume queued output. Connect/configure/close still depend on the
driver; nonblocking flags and application deadlines cannot guarantee timing of a
faulty kernel driver or OS scheduler. Worker destruction joins after its current
operation returns, so keep teardown outside the audio callback, as before.

The implementation follows Apple's
[serial-device POSIX setup example](https://developer.apple.com/documentation/iokit/communicating_with_a_modem_on_a_serial_port)
and [Darwin termios definitions](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/termios.h).
Unlike the blocking/draining modem example, this backend retains O_NONBLOCK and
does not drain on close. All opens, configuration, reads, writes and closes remain
on the device worker. Protocol parsing, queue semantics and APVTS servicing do not
move to the audio thread.

## Build and test on a Mac

From a Mac checkout with the JUCE submodule initialized and Xcode selected:

```sh
cmake -S . -B build -G Xcode
cmake --build build --config Debug --parallel 4
ctest --test-dir build -C Debug --output-on-failure
```

For a targeted native-backend check:

```sh
cmake --build build --config Debug --target MixSenseMacSerialTests
ctest --test-dir build -C Debug -V -R native_serial_macos
```

On macOS, CTest adds four independently reported cases to the four cross-platform
suites: `native_serial_macos_configuration`, `native_serial_macos_binary_io`,
`native_serial_macos_cleanup`, and `native_serial_macos_backpressure_hangup`.
Each case runs in its own process with its own pseudoterminal, so a failure does
not prevent the remaining cases from running. Running `MixSenseMacSerialTests`
without arguments also runs all cases and reports any failures before exiting.
These checks use pseudoterminals, not an ESP32-S3, to test the actual termios
configuration, raw binary input, partial I/O, output backpressure, hangup, settings restoration
and reopening after close. A narrow internal test accessor adopts the PTY
slave because PTY names are `/dev/ttys*`; production port validation is not weakened.
Successful backend configuration requires the TIOCEXCL syscall to succeed, but
Darwin PTYs do not reliably reject subsequent non-root opens. PTY reopening does
not prove exclusive ownership or its release; verify competing-instance exclusion
and release on a physical `/dev/cu.*` CDC device. Kernel timing checks allow
scheduling slack and do not certify audio deadlines. These Mac-only cases have
passed on Apple Silicon; physical USB CDC behavior remains unverified.

The cross-platform `serial_port_names` suite checks name normalization/validation
and the selected native factory's closed/invalid-path behavior. Existing simulated
CDC tests now also check a mixed-case Mac path passing through the application,
without accessing a real device.

## Remaining Mac bench checks

1. Extend the verified Apple Silicon Debug build and eight passing CTest cases to
   any additional CPU architectures, configurations and deployment targets you
   intend to ship. Universal builds and distribution signing/notarization remain
   unverified.
2. Select an actual `/dev/cu.*` name in the editor, close competing serial tools,
   and connect. No automatic discovery is implemented. Confirm permissions and
   DTR/reset behavior with the ESP32-S3 firmware team.
3. Exercise the existing snapshot/movement protocol and persistent synthetic levels.
   Confirm decoded bytes and APVTS changes, without changing packet layout.
4. Repeat inside the intended DAW. Confirm serial access and exclusive-port failure
   for a second instance without disrupting the first connection or audio.
5. During audio playback and continuous traffic, unplug/reconnect and close the
   plugin. Check driver error/EOF reporting, timely worker teardown, no old pulses
   or levels replaying, and the firmware watchdog independently zeroing output.

The passing macOS build and hardware-free tests verify compilation and host-side
behavior on Apple Silicon. Physical USB-driver behavior, DAW integration, actual
audio dropout performance and actuator operation still require bench testing.
