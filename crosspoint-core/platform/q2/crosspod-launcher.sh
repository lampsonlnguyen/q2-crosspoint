#!/bin/sh
# SPDX-License-Identifier: MIT
# Q2 Pod execl() launches this directly as .crosspod/q2-crosspod.
# /bin/sh exists on stock Q2; a script lets us record exec failures even
# when the MIPS reader ELF cannot start (ABI, EACCES, ENOEXEC, missing loader).
ROOT=${CROSSPOD_APP_DIR:-/mnt/mmc/.crosspod}
LOG="$ROOT/startup.log"
BIN="$ROOT/crosspod-engine"
# Keep Section HTML/page caches and font settings on SD, not the 128MB tmpfs.
export CROSSPOD_STATE_DIR="$ROOT"
# Do not block launch if card is read-only. Keep log size bounded across boots.
if ( : >> "$LOG" ) 2>/dev/null; then
  if [ "$(wc -c < "$LOG" 2>/dev/null)" -gt 65536 ] 2>/dev/null; then
    mv -f "$LOG" "$ROOT/startup.previous.log" 2>/dev/null || :
    : > "$LOG" 2>/dev/null || :
  fi
  {
    echo "===== CrossPod launch ====="
    date -u '+UTC %Y-%m-%dT%H:%M:%SZ' 2>/dev/null || :
    uname -a 2>/dev/null || :
    echo "runner=/bin/sh"
    ls -l "$BIN" 2>/dev/null || echo "crosspod-engine missing"
    grep -E '^(MemAvailable|MemFree|Shmem):' /proc/meminfo 2>/dev/null || :
    echo "starting crosspod-engine"
  } >> "$LOG" 2>&1
  exec "$BIN" >> "$LOG" 2>&1
else
  exec "$BIN"
fi
