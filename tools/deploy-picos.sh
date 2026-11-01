#!/usr/bin/env bash
set -Eeuo pipefail

MODE="${1:-status}"
PICO1_UF2="${2:-}"
PICO2W_UF2="${3:-}"

PICO1_LABEL="RPI-RP2"
PICO2W_LABEL="RP2350"
RUNTIME_IDS_RE='ID cafe:(babe|babf)'

if [[ "$MODE" != "status" && "$MODE" != "deploy" ]]; then
  echo "Usage: $0 {status|deploy} [pico1.uf2 pico2w.uf2]" >&2
  exit 2
fi

if [[ "$MODE" == "deploy" ]]; then
  [[ -f "$PICO1_UF2" ]] || { echo "Missing Pico 1 UF2: $PICO1_UF2" >&2; exit 1; }
  [[ -f "$PICO2W_UF2" ]] || { echo "Missing Pico 2 W UF2: $PICO2W_UF2" >&2; exit 1; }
  if (( EUID != 0 )); then
    echo "Deployment needs root privileges to mount BOOTSEL volumes." >&2
    echo "Run: sudo make deploy" >&2
    exit 1
  fi
fi

boot_partitions() {
  local wanted_label="$1"
  lsblk -rpn -o NAME,LABEL,TYPE 2>/dev/null |
    awk -v wanted="$wanted_label" '$2 == wanted && $3 == "part" { print $1 }'
}

runtime_device_count() {
  lsusb 2>/dev/null | awk '$5 == "ID" && ($6 == "cafe:babe" || $6 == "cafe:babf") { count++ } END { print count+0 }'
}

line_count() {
  local value="${1:-}"
  if [[ -z "$value" ]]; then
    echo 0
  else
    printf '%s\n' "$value" | awk 'NF { n++ } END { print n+0 }'
  fi
}

flash_volume() {
  local device="$1"
  local board_name="$2"
  local uf2="$3"
  local mount_dir=""
  local created_mount=0
  local existing_mount=""

  if [[ ! -b "$device" ]]; then
    echo "[$board_name] BOOTSEL device disappeared: $device" >&2
    return 1
  fi

  existing_mount="$(findmnt -rn -S "$device" -o TARGET 2>/dev/null | head -n 1 || true)"
  if [[ -n "$existing_mount" ]]; then
    mount_dir="$existing_mount"
  else
    mount_dir="$(mktemp -d "/run/pico-dac2-${board_name}.XXXXXX")"
    created_mount=1
    if ! mount "$device" "$mount_dir"; then
      rmdir "$mount_dir" 2>/dev/null || true
      echo "[$board_name] Could not mount $device" >&2
      return 1
    fi
  fi

  if [[ ! -d "$mount_dir" || ! -w "$mount_dir" ]] || ! ls -A "$mount_dir" >/dev/null 2>&1; then
    if (( created_mount )); then
      umount "$mount_dir" 2>/dev/null || true
      rmdir "$mount_dir" 2>/dev/null || true
    fi
    echo "[$board_name] BOOTSEL mount is unavailable or stale at $mount_dir" >&2
    echo "Clear the stale mount and run 'make deploy' again; no UF2 was copied." >&2
    return 1
  fi

  echo "[$board_name] Flashing $uf2 to $device ..."
  if ! cp -- "$uf2" "$mount_dir/mdac_adc2.uf2"; then
    if (( created_mount )); then
      umount "$mount_dir" 2>/dev/null || true
      rmdir "$mount_dir" 2>/dev/null || true
    fi
    echo "[$board_name] Copy failed." >&2
    return 1
  fi

  sync
  if (( created_mount )); then
    # BOOTSEL normally disconnects itself after accepting a UF2. If it has
    # not disconnected, clean up the temporary mount normally.
    if findmnt -rn -M "$mount_dir" >/dev/null 2>&1; then
      umount "$mount_dir" 2>/dev/null || true
    fi
    rmdir "$mount_dir" 2>/dev/null || true
  fi

  echo "[$board_name] UF2 copied successfully. The board should reboot."
}

show_status() {
  local pico1 pico2w running
  pico1="$(boot_partitions "$PICO1_LABEL")"
  pico2w="$(boot_partitions "$PICO2W_LABEL")"
  running="$(runtime_device_count)"

  echo "Pico 1 (RP2040) BOOTSEL partitions ($PICO1_LABEL):"
  if [[ -n "$pico1" ]]; then printf '  %s\n' "$pico1"; else echo '  none'; fi
  echo "Pico 2 W (RP2350) BOOTSEL partitions ($PICO2W_LABEL):"
  if [[ -n "$pico2w" ]]; then printf '  %s\n' "$pico2w"; else echo '  none'; fi
  echo "Running pico-dac2 firmware devices (cafe:babe / cafe:babf): $running"
  echo "Note: runtime firmware uses the same USB IDs on both models, so silicon type is only unambiguous in BOOTSEL."
}

if [[ "$MODE" == "status" ]]; then
  show_status
  exit 0
fi

flashed_pico1=0
flashed_pico2w=0
expected_total=0

while true; do
  pico1_devices="$(boot_partitions "$PICO1_LABEL")"
  pico2w_devices="$(boot_partitions "$PICO2W_LABEL")"
  pico1_count="$(line_count "$pico1_devices")"
  pico2w_count="$(line_count "$pico2w_devices")"
  running_count="$(runtime_device_count)"
  visible_total=$((pico1_count + pico2w_count + running_count))
  if (( visible_total > expected_total )); then
    expected_total="$visible_total"
  fi

  did_flash=0

  if (( pico1_count > 0 && flashed_pico1 == 0 )); then
    while IFS= read -r device; do
      [[ -n "$device" ]] || continue
      flash_volume "$device" "pico1-rp2040" "$PICO1_UF2"
      flashed_pico1=1
      did_flash=1
      break
    done <<< "$pico1_devices"
  fi

  if (( pico2w_count > 0 && flashed_pico2w == 0 )); then
    while IFS= read -r device; do
      [[ -n "$device" ]] || continue
      flash_volume "$device" "pico2w-rp2350" "$PICO2W_UF2"
      flashed_pico2w=1
      did_flash=1
      break
    done <<< "$pico2w_devices"
  fi

  # One of each board is the intended deployment pair. The total includes
  # BOOTSEL volumes and runtime devices, so a board that reboots after flashing
  # is not mistaken for a second, unflashed board.
  flashed_count=$((flashed_pico1 + flashed_pico2w))
  if (( expected_total < flashed_count )); then
    expected_total="$flashed_count"
  fi
  remaining=$((expected_total - flashed_count))
  if (( remaining <= 0 )); then
    if (( flashed_count > 0 )); then
      echo "Deployment complete: flashed $flashed_count board(s)."
      exit 0
    fi
  fi

  if (( did_flash )); then
    sleep 3
    continue
  fi

  if (( remaining > 0 )); then
    echo
    echo "Detected $running_count running firmware device(s), and $((pico1_count + pico2w_count)) BOOTSEL volume(s)."
    echo "$remaining board(s) still need to be placed in BOOTSEL."
    echo "Hold BOOTSEL while reconnecting each remaining board."
    echo "Pico 1 appears as the RPI-RP2 volume; Pico 2 W appears as RP2350."
    read -r -p "Press Enter after putting the next board (or both boards) into BOOTSEL, or Ctrl-C to cancel. " _
    continue
  fi

  if (( running_count == 0 && pico1_count == 0 && pico2w_count == 0 )); then
    echo "No supported Pico running this firmware or in BOOTSEL was detected." >&2
    echo "Connect the Pico 1 and/or Pico 2 W, then run 'make deploy' again." >&2
    exit 1
  fi

done
