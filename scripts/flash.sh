#!/bin/bash
# Run on your Mac host (not inside Docker) — requires openocd in PATH.
set -e

ELF=${1:-app/build/zephyr/zephyr.elf}
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "${SCRIPT_DIR}/${ELF}" ]; then
  echo "Error: ${ELF} not found. Build first with: docker compose run zephyr ./scripts/build.sh"
  exit 1
fi

openocd \
  -f "${SCRIPT_DIR}/openocd/picoprobe.cfg" \
  -c "program ${SCRIPT_DIR}/${ELF} verify reset exit"
