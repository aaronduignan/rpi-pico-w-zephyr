#!/bin/bash
# Run inside the container: docker compose run --rm zephyr ./scripts/build.sh
set -e

JOBS=${JOBS:-$(nproc)}
PRISTINE=${PRISTINE:-auto}
SAMPLE=${SAMPLE:-ble_advertiser}
CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-5G}

APP_DIR=/workspace/app/samples/${SAMPLE}
BUILD_DIR=/workspace/app/build

export ZEPHYR_BASE=/workspace/zephyr
if command -v ccache >/dev/null 2>&1; then
    ccache --max-size="${CCACHE_MAXSIZE}" >/dev/null
fi
cd /workspace
west build -b rpi_pico/rp2040/w "${APP_DIR}" --build-dir "${BUILD_DIR}" --pristine="${PRISTINE}" --build-opt=-j"${JOBS}" "$@"
echo "Build complete: build/zephyr/zephyr.elf"
python3 /workspace/scripts/elf_map.py --short "${BUILD_DIR}/zephyr/zephyr.elf"
