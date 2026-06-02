#!/bin/bash
# Run inside the container: docker compose run zephyr ./scripts/build.sh
set -e

JOBS=${JOBS:-4}

export ZEPHYR_BASE=/workspace/zephyr
cd /workspace
west build -b rpi_pico/rp2040/w /workspace/app --build-dir /workspace/app/build --pristine=auto --build-opt=-j"${JOBS}" "$@"
echo "Build complete: app/build/zephyr/zephyr.elf"
