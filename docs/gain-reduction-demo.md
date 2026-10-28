# Gain-reduction demo

The existing MixSense editor now includes a basic compressor and a meter of its
actual applied gain reduction. The meter is not estimated from the threshold or
FSR reading. With no audio input and the test tone off, it should show zero GR.

## Quick standalone test

1. Build and open the standalone app using [building.md](building.md).
2. Leave Bypass off. Enable **Test tone (-18 dBFS)**. This supplies a 1 kHz sine
   wave to the compressor in place of the input; it can be audible at the selected
   output. Start with a low speaker/headphone volume.
3. Set Threshold to about -36 dB. The orange meter should rise and show a positive
   value. Lowering the threshold produces more reduction for the same signal.
4. Raise Threshold to 0 dB: GR releases toward zero. Enable Bypass: GR immediately
   reads zero and the audio is dry. Disable the test tone after testing.

If the input status says **Audio inactive**, the app is not receiving audio callbacks.
Check its audio device settings. **Input: silence** means callbacks are running but
the incoming signal is effectively silent. The meter updates around 30 times per
second and clears after 250 ms without a new audio publication.

For real audio, disable the test tone and route audio into the app's selected input,
or load the rebuilt plugin on an audio track in a DAW. System playback is not
automatically routed into this effect. The tone defaults off, is not stored in
presets, and is disabled when the editor closes.

## Prototype DSP

`dsp/DemoCompressor.h` is a hard-knee, stereo-linked feed-forward compressor.
It detects the largest absolute input sample across channels, computes the target
reduction from threshold/ratio, smooths the reduction in dB with attack/release,
and applies the same gain to every channel. Makeup gain is applied afterwards.
The meter reports the maximum applied reduction in each audio block, before makeup.
Its bar spans 0–24 dB; the numeric readout can exceed 24 dB.

Threshold, ratio, attack, release, makeup and bypass use the existing APVTS
parameters. The editor exposes threshold/bypass and displays ratio/attack/release;
the other controls remain available through host parameters. This is a bench DSP
prototype, without a soft knee, lookahead, oversampling or output limiter.

Audio processing performs no serial I/O or UI work. `AudioMeterState` publishes
coherent atomics with a lifetime generation for UI and later haptic mapping.
`compressor_dsp_and_meter` tests the attenuation formula, stereo linking, makeup,
bypass, attack/release, concurrent meter reads and the processor's test tone.

## FSR and ERM integration

Use the `mixsense_gr_erm` PlatformIO environment for combined FSR input, v1 USB
protocol, and Adafruit ERM output. Follow the
[combined bench procedure](../firmware/esp32s3/GR_ERM.md). More pressure lowers the
threshold in this profile. Startup snapshots remain informational; movement edits
the threshold. The older `adafruit_erm` environment remains a standalone sweep.

After connecting CDC, **Enable GR haptics (ERM)** explicitly selects measured GR
as the continuous feedback source. The synthetic panel is disabled while it is
selected. GR haptics defaults off, is not stored in presets, turns off when the
editor closes or the serial session changes, and requires a new audio publication
after activation. Stop/zero also disables GR haptics.

The provisional mapping is off at or below 0.5 dB GR; above that, drive rises
linearly from RTP 30 to 90 as GR approaches 18 dB. Larger GR is capped. Effective
silence (input below -90 dBFS) requests zero even during a compressor release tail.
Both the host mapping and this firmware cap positive signed RTP at 90/127.
The minimum starting drive and perceived strength still need physical calibration.

The message-thread service consumes only new audio generations. It refreshes a
held nonzero level at 50 ms, with prompt updates when it changes, and expires the
source after 250 ms without a new publication. The MCU separately stops after
1000 ms without valid haptic commands, or on USB session changes. Serial work
stays off the audio thread. Bus/configuration faults latch the motor output off
where I2C writes can still reach the driver; no hardware EN shutdown is wired.

## Why -36 dB threshold and -18 dBFS input show about 12 dB GR

For a constant detector level above the threshold, with ratio R:

```text
GR = (input_dB - threshold_dB) * (1 - 1/R)
output_dB = input_dB - GR        (before makeup)

(-18 - -36) * (1 - 1/4) = 18 * 0.75 = 13.5 dB GR
output = -18 - 13.5 = -31.5 dB
```

The test tone is a sine whose **peak** is -18 dBFS. Most samples are below that
peak. This prototype calculates its target reduction from individual sample
magnitudes and then smooths reduction in dB with attack/release. The measured
applied reduction therefore settles around 12.3 dB for the 1 kHz tone with the
default 10 ms attack / 100 ms release. It is not a constant -18 dB detector level.
The automated tests cover both the constant-level 13.5 dB case and this sine case.
The meter displays a positive reduction amount; 12 dB reduction corresponds to
-12 dB of applied compressor gain before makeup. A different detector design can
produce a different steady sine reading.
