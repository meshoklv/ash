#!/usr/bin/env bash
# Run a command holding one of N emulator slots (default 3): emu-slot.sh <cmd...>
# Waits while MemAvailable < EMU_MIN_FREE_MB (default 3000) so the shared host is not starved.
# PPSSPPHeadless must still use the shm shim (see scripts/shoot-game.sh).
n="${EMU_SLOTS:-3}"; min="${EMU_MIN_FREE_MB:-3000}"
avail() { awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo; }
while :; do
  if [ "$(avail)" -ge "$min" ]; then
    for i in $(seq 0 $((n-1))); do
      exec 9>"$HOME/ash-assets/.emu.slot.$i"
      if flock -n 9; then exec nice -n 15 "$@"; fi
    done
  fi
  sleep 3
done
