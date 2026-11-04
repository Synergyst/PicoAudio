#!/usr/bin/env bash
set -euo pipefail
cat >&2 <<'MSG'
The old PCM loopback test is intentionally disabled in the Stage-1 C diagnostic build.
This build verifies GPIO data with a pre-UAC startup challenge/response; it does not
forward PCM, so ALSA captures are expected to be silent.

Use:
  ./tools/check-gpio-c-diagnostic.sh

The actual bit-level test runs automatically on both boards before UAC2 enumerates.
MSG
exit 2
