#!/bin/bash
# Run inside the container: docker compose run zephyr ./scripts/build.sh
set -e

JOBS=${JOBS:-$(nproc)}
PRISTINE=${PRISTINE:-auto}

export ZEPHYR_BASE=/workspace/zephyr
cd /workspace
west build -b rpi_pico/rp2040/w /workspace/app --build-dir /workspace/app/build --pristine="${PRISTINE}" --build-opt=-j"${JOBS}" "$@"
echo "Build complete: app/build/zephyr/zephyr.elf"
python3 /workspace/scripts/elf_map.py --short /workspace/app/build/zephyr/zephyr.elf
