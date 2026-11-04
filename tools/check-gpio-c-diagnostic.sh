#!/usr/bin/env bash
set -Eeuo pipefail

if ! command -v lsusb >/dev/null 2>&1; then
  echo "ERROR: lsusb is required (install usbutils)." >&2
  exit 2
fi

mapfile -t devices < <(lsusb -d cafe:babe)
count=${#devices[@]}

echo "PicoAudio C diagnostic USB check"
echo "  Expected: 2 cafe:babe devices"
echo "  Found:    $count"
printf '%s\n' "${devices[@]}"

if (( count != 2 )); then
  echo "FAIL: both boards have not reached post-diagnostic UAC2 initialization." >&2
  echo "Check both UART logs for the GPIO C diagnostic result." >&2
  exit 1
fi

echo "PASS: both board USB audio devices enumerated after the firmware startup diagnostic."
echo "Note: Stage 1 does not transport PCM; audio loopback is intentionally disabled."
