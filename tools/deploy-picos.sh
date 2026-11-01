#!/usr/bin/env bash
set -Eeuo pipefail

MODE="${1:-status}"
REQUESTED_BOARD="${BOARD:-}"
REQUESTED_ROLE="${ROLE:-}"
REQUESTED_DEVICE="${DEVICE:-}"
JOBS="${JOBS:-4}"
RX_DATA_PIN="${RX_DATA_PIN:-26}"
CMAKE_BIN="${CMAKE:-cmake}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

usage() {
  cat <<'USAGE'
Usage:
  make deploy [BOARD=pico1|pico2|pico2w] [ROLE=master|slave] [DEVICE=/dev/sdX1]
  make status

If BOARD is omitted, the RPI-RP2 label identifies Pico 1 automatically.
An RP2350 BOOTSEL volume requires a Pico 2 vs Pico 2 W choice because both
models expose the same RP2350 boot volume label. ROLE is prompted unless set.
USAGE
}

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

prompt_value() {
  local prompt_text="$1"
  local answer
  [[ -r /dev/tty ]] || fail "Interactive selection requires a terminal. Specify BOARD= and ROLE= explicitly."
  printf '%s' "$prompt_text" > /dev/tty
  IFS= read -r answer < /dev/tty || fail "Could not read selection from terminal."
  printf '%s' "$answer"
}

normalize_board() {
  case "${1,,}" in
    pico1|pico|rp2040) printf 'pico1' ;;
    pico2|rp2350|pico-2) printf 'pico2' ;;
    pico2w|pico2_w|pico-2-w|pico2-w) printf 'pico2w' ;;
    *) return 1 ;;
  esac
}

normalize_role() {
  case "${1,,}" in
    master|m) printf 'master' ;;
    slave|s) printf 'slave' ;;
    *) return 1 ;;
  esac
}

expected_label_for_board() {
  case "$1" in
    pico1) printf 'RPI-RP2' ;;
    pico2|pico2w) printf 'RP2350' ;;
    *) return 1 ;;
  esac
}

build_dir_for() {
  case "$1-$2" in
    pico1-master) printf 'build-pico1' ;;
    pico1-slave) printf 'build-pico1-slave' ;;
    pico2-master) printf 'build-pico2-master' ;;
    pico2-slave) printf 'build-pico2-slave' ;;
    pico2w-master) printf 'build-pico2w' ;;
    pico2w-slave) printf 'build-pico2w-slave' ;;
    *) return 1 ;;
  esac
}

boot_entries() {
  lsblk -rpn -o NAME,LABEL,TYPE 2>/dev/null |
    awk '$3 == "part" && ($2 == "RPI-RP2" || $2 == "RP2350") { print $1 "|" $2 }'
}

runtime_devices() {
  lsusb 2>/dev/null | awk '/ID cafe:(babe|babf)/ { print }' || true
}

show_status() {
  local entries running
  entries="$(boot_entries || true)"
  running="$(runtime_devices)"

  echo 'PicoAudio BOOTSEL devices:'
  if [[ -n "$entries" ]]; then
    while IFS='|' read -r device label; do
      [[ -n "$device" ]] || continue
      case "$label" in
        RPI-RP2) printf '  %-12s %-8s %s\n' "$device" "$label" 'Pico 1 / RP2040' ;;
        RP2350) printf '  %-12s %-8s %s\n' "$device" "$label" 'Pico 2 or Pico 2 W / RP2350 (model indistinguishable in BOOTSEL)' ;;
      esac
    done <<< "$entries"
  else
    echo '  none'
  fi

  echo 'Running PicoAudio USB devices:'
  if [[ -n "$running" ]]; then
    printf '%s\n' "$running" | sed 's/^/  /'
  else
    echo '  none'
  fi
  echo 'Note: the current USB descriptors do not identify the board model or I2S role after boot.'
}

select_boot_device() {
  local entries=() filtered=() line dev label expected answer index
  mapfile -t entries < <(boot_entries)

  if [[ -n "$REQUESTED_BOARD" ]]; then
    REQUESTED_BOARD="$(normalize_board "$REQUESTED_BOARD")" || fail "Unknown BOARD. Use pico1, pico2, or pico2w."
    expected="$(expected_label_for_board "$REQUESTED_BOARD")"
    for line in "${entries[@]}"; do
      IFS='|' read -r dev label <<< "$line"
      [[ "$label" == "$expected" ]] && filtered+=("$line")
    done
  else
    filtered=("${entries[@]}")
  fi

  ((${#filtered[@]} > 0)) || {
    echo 'No matching Pico BOOTSEL partition was found.' >&2
    echo 'Detected BOOTSEL partitions:' >&2
    if ((${#entries[@]})); then printf '  %s\n' "${entries[@]}" >&2; else echo '  none' >&2; fi
    echo 'Hold BOOTSEL while reconnecting the board, then run make deploy again.' >&2
    exit 1
  }

  if [[ -n "$REQUESTED_DEVICE" ]]; then
    for line in "${filtered[@]}"; do
      IFS='|' read -r dev label <<< "$line"
      [[ "$dev" == "$REQUESTED_DEVICE" ]] && { printf '%s|%s' "$dev" "$label"; return; }
    done
    fail "DEVICE=$REQUESTED_DEVICE is not among the matching BOOTSEL partitions."
  fi

  if ((${#filtered[@]} == 1)); then
    printf '%s' "${filtered[0]}"
    return
  fi

  printf 'Multiple BOOTSEL volumes are connected:\n' > /dev/tty
  for index in "${!filtered[@]}"; do
    IFS='|' read -r dev label <<< "${filtered[$index]}"
    printf '  %d) %s (%s)\n' "$((index + 1))" "$dev" "$label" > /dev/tty
  done
  answer="$(prompt_value 'Select the volume belonging to the physical board you want to flash: ')"
  [[ "$answer" =~ ^[0-9]+$ ]] || fail 'Selection must be a number.'
  (( answer >= 1 && answer <= ${#filtered[@]} )) || fail 'Selection is out of range.'
  printf '%s' "${filtered[$((answer - 1))]}"
}

select_board() {
  local detected_label="$1" choice
  if [[ -n "$REQUESTED_BOARD" ]]; then
    REQUESTED_BOARD="$(normalize_board "$REQUESTED_BOARD")" || fail 'BOARD must be pico1, pico2, or pico2w.'
    [[ "$(expected_label_for_board "$REQUESTED_BOARD")" == "$detected_label" ]] ||
      fail "BOARD=$REQUESTED_BOARD does not match BOOTSEL label $detected_label."
    printf '%s' "$REQUESTED_BOARD"
    return
  fi

  if [[ "$detected_label" == 'RPI-RP2' ]]; then
    printf 'pico1'
    return
  fi

  echo 'The RP2350 BOOTSEL label is shared by Pico 2 and Pico 2 W.' > /dev/tty
  echo 'Which physical board is currently in BOOTSEL?' > /dev/tty
  echo '  1) Raspberry Pi Pico 2 (non-W)' > /dev/tty
  echo '  2) Raspberry Pi Pico 2 W' > /dev/tty
  choice="$(prompt_value 'Select 1 or 2: ')"
  case "$choice" in
    1) printf 'pico2' ;;
    2) printf 'pico2w' ;;
    *) fail 'Board selection must be 1 or 2.' ;;
  esac
}

select_role() {
  local choice
  if [[ -n "$REQUESTED_ROLE" ]]; then
    REQUESTED_ROLE="$(normalize_role "$REQUESTED_ROLE")" || fail 'ROLE must be master or slave.'
    printf '%s' "$REQUESTED_ROLE"
    return
  fi

  echo 'Which I2S clock role should this firmware use?' > /dev/tty
  echo '  1) Master (generates BCLK and LRCLK)' > /dev/tty
  echo '  2) Slave (receives BCLK and LRCLK)' > /dev/tty
  choice="$(prompt_value 'Select 1 or 2: ')"
  case "$choice" in
    1) printf 'master' ;;
    2) printf 'slave' ;;
    *) fail 'Role selection must be 1 or 2.' ;;
  esac
}

flash_volume() {
  local device="$1" board="$2" role="$3" uf2="$4"
  local mount_dir='' created_mount=0 existing_mount=''

  [[ -b "$device" ]] || fail "BOOTSEL partition disappeared: $device"
  existing_mount="$(findmnt -rn -S "$device" -o TARGET 2>/dev/null | head -n 1 || true)"

  if [[ -n "$existing_mount" ]]; then
    mount_dir="$existing_mount"
  else
    mount_dir="$(mktemp -d "/run/picoaudio-${board}-${role}.XXXXXX")"
    created_mount=1
    if ! mount "$device" "$mount_dir"; then
      rmdir "$mount_dir" 2>/dev/null || true
      fail "Could not mount $device. No UF2 was copied."
    fi
  fi

  if [[ ! -d "$mount_dir" || ! -w "$mount_dir" ]] || ! ls -A "$mount_dir" >/dev/null 2>&1; then
    if (( created_mount )); then
      umount "$mount_dir" 2>/dev/null || true
      rmdir "$mount_dir" 2>/dev/null || true
    fi
    fail "BOOTSEL mount is unavailable or stale at $mount_dir. No UF2 was copied."
  fi

  echo "[$board / $role] Flashing $uf2 to $device ..."
  if ! cp -- "$uf2" "$mount_dir/mdac_adc2.uf2"; then
    if (( created_mount )); then
      umount "$mount_dir" 2>/dev/null || true
      rmdir "$mount_dir" 2>/dev/null || true
    fi
    fail "UF2 copy failed for $device."
  fi
  sync

  if (( created_mount )); then
    if findmnt -rn -M "$mount_dir" >/dev/null 2>&1; then
      umount "$mount_dir" 2>/dev/null || true
    fi
    rmdir "$mount_dir" 2>/dev/null || true
  fi

  echo "[$board / $role] UF2 copied successfully. The board should reboot."
}

run_deploy() {
  local selected device label board role build_dir uf2

  (( EUID == 0 )) || fail "Deployment requires root to mount BOOTSEL volumes. Run: sudo -E make deploy"
  selected="$(select_boot_device)"
  IFS='|' read -r device label <<< "$selected"
  board="$(select_board "$label")"
  role="$(select_role)"
  build_dir="$(build_dir_for "$board" "$role")"

  echo "Selected target: board=$board, role=$role, BOOTSEL=$device ($label)"
  echo 'Building the matching firmware image...'
  cd "$PROJECT_ROOT"
  make --no-print-directory build-one \
    BOARD="$board" ROLE="$role" JOBS="$JOBS" \
    RX_DATA_PIN="$RX_DATA_PIN" CMAKE="$CMAKE_BIN"

  uf2="$PROJECT_ROOT/$build_dir/mdac_adc2.uf2"
  [[ -f "$uf2" ]] || fail "Build completed without producing $uf2"
  flash_volume "$device" "$board" "$role" "$uf2"
}

case "$MODE" in
  status)
    show_status
    ;;
  deploy)
    run_deploy
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
