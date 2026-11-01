#!/usr/bin/env bash
set -Eeuo pipefail

# Usage: tools/test-gpio-bitbang.sh [SLAVE_ALSA_CARD] [MASTER_ALSA_CARD]
# Defaults match the current CM5 enumeration described in the project notes.
SLAVE_CARD="${1:-2}"
MASTER_CARD="${2:-5}"
DURATION_SECONDS=14
PLAY_SECONDS=10
REC_SLAVE="/tmp/gpio-link-slave-capture.wav"
REC_MASTER="/tmp/gpio-link-master-capture.wav"
LOG_DIR="/tmp/gpio-link-test-logs"

for tool in arecord speaker-test timeout; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ERROR: required command '$tool' is not installed." >&2
    exit 2
  }
done

[[ "$SLAVE_CARD" =~ ^[0-9]+$ ]] || { echo "Invalid slave card number: $SLAVE_CARD" >&2; exit 2; }
[[ "$MASTER_CARD" =~ ^[0-9]+$ ]] || { echo "Invalid master card number: $MASTER_CARD" >&2; exit 2; }
(( SLAVE_CARD != MASTER_CARD )) || { echo "Slave and master ALSA cards must differ." >&2; exit 2; }

mkdir -p "$LOG_DIR"
rm -f "$REC_SLAVE" "$REC_MASTER"

cleanup() {
  local pid
  for pid in "${REC_SLAVE_PID:-}" "${REC_MASTER_PID:-}" "${PLAY_SLAVE_PID:-}" "${PLAY_MASTER_PID:-}"; do
    [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

echo "GPIO bitbang full-duplex test"
echo "  Top/slave ALSA card:    $SLAVE_CARD"
echo "  Bottom/master ALSA card: $MASTER_CARD"
echo "  Capture duration:       ${DURATION_SECONDS}s"
echo "  Playback duration:      ${PLAY_SECONDS}s"
echo

# Open both capture endpoints before producing either playback stream.
arecord -q -D "hw:${SLAVE_CARD},0" -f S32_LE -r 48000 -c 2 \
  -d "$DURATION_SECONDS" "$REC_SLAVE" \
  >"$LOG_DIR/record-slave.log" 2>&1 &
REC_SLAVE_PID=$!

arecord -q -D "hw:${MASTER_CARD},0" -f S32_LE -r 48000 -c 2 \
  -d "$DURATION_SECONDS" "$REC_MASTER" \
  >"$LOG_DIR/record-master.log" 2>&1 &
REC_MASTER_PID=$!

# This fixed startup delay is machine-controlled, not operator timing.
sleep 1

if ! kill -0 "$REC_SLAVE_PID" 2>/dev/null || ! kill -0 "$REC_MASTER_PID" 2>/dev/null; then
  echo "ERROR: one or both capture streams failed to open." >&2
  cat "$LOG_DIR/record-slave.log" "$LOG_DIR/record-master.log" >&2
  exit 1
fi

# Simultaneous, distinct test tones in opposite directions.
timeout --signal=INT --kill-after=1s "${PLAY_SECONDS}s" \
  speaker-test -D "hw:${SLAVE_CARD},0" -F S32_LE -r 48000 -c 2 \
  -t sine -f 400 >"$LOG_DIR/play-slave.log" 2>&1 &
PLAY_SLAVE_PID=$!

timeout --signal=INT --kill-after=1s "${PLAY_SECONDS}s" \
  speaker-test -D "hw:${MASTER_CARD},0" -F S32_LE -r 48000 -c 2 \
  -t sine -f 1000 >"$LOG_DIR/play-master.log" 2>&1 &
PLAY_MASTER_PID=$!

# Capture both speaker-test exit codes; timeout status 124/130 is expected.
wait "$PLAY_SLAVE_PID" || PLAY_SLAVE_STATUS=$?
wait "$PLAY_MASTER_PID" || PLAY_MASTER_STATUS=$?
PLAY_SLAVE_STATUS="${PLAY_SLAVE_STATUS:-0}"
PLAY_MASTER_STATUS="${PLAY_MASTER_STATUS:-0}"

wait "$REC_SLAVE_PID" || REC_SLAVE_STATUS=$?
wait "$REC_MASTER_PID" || REC_MASTER_STATUS=$?
REC_SLAVE_STATUS="${REC_SLAVE_STATUS:-0}"
REC_MASTER_STATUS="${REC_MASTER_STATUS:-0}"

trap - EXIT INT TERM

echo
echo "==================== RESULTS ===================="
echo "Slave capture status:  $REC_SLAVE_STATUS"
echo "Master capture status: $REC_MASTER_STATUS"
echo "Slave playback status: $PLAY_SLAVE_STATUS (124/130 expected from timeout)"
echo "Master playback status: $PLAY_MASTER_STATUS (124/130 expected from timeout)"
echo
echo "Expected incoming signals:"
echo "  $REC_SLAVE  <- 1000 Hz from master playback"
echo "  $REC_MASTER <-  400 Hz from slave playback"
echo
echo "Recordings:"
ls -lh "$REC_SLAVE" "$REC_MASTER"
echo
echo "Logs: $LOG_DIR"
echo "Attach both WAV files for signal inspection."
