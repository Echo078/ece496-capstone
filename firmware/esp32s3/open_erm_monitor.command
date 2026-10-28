#!/bin/zsh
set -e

cd "${0:A:h}"
serial_port=${1:-/dev/cu.usbmodem348518B5D36C1}
pio_executable=$(command -v pio || true)
if [[ -z "$pio_executable" ]]; then
    pio_executable="$HOME/.platformio/penv/bin/pio"
fi

print -- "ERM vibration monitor"
print -- "Wait for READY and follow the firmware instructions below. Type 0 to stop."
print -- "simple_erm: 1 = one second. adafruit_erm: 1 = one 14-second level sweep."
print -- "Ctrl+C closes the monitor."
print -- "Port: $serial_port"

if [[ ! -c "$serial_port" ]]; then
    print -- "ESP32 application port is missing. Press RESET once (not BOOT), then reopen this file."
    read -k 1 "?Press any key to close."
    exit 1
fi
if [[ ! -x "$pio_executable" ]]; then
    print -- "PlatformIO was not found. Open this project in PlatformIO and choose Monitor."
    read -k 1 "?Press any key to close."
    exit 1
fi

exec "$pio_executable" device monitor --port "$serial_port" --baud 115200 \
    --rts 0 --dtr 1 --echo --eol LF
