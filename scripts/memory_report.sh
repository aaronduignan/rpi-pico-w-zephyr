#!/bin/bash
# Print ROM and RAM usage reports for the current build.
# Run inside the container: docker compose run --rm zephyr ./scripts/memory_report.sh
#
# For the full per-symbol tree add --verbose:
#   docker compose run --rm zephyr ./scripts/memory_report.sh --verbose
set -e

BUILD_DIR=/workspace/app/build
ZEPHYR_BASE=/workspace/zephyr
export ZEPHYR_BASE

cd /workspace

if [[ "$1" == "--verbose" ]]; then
    echo "=== ROM report ==="
    west build -b rpi_pico/rp2040/w /workspace/app --build-dir "${BUILD_DIR}" -t rom_report

    echo ""
    echo "=== RAM report ==="
    west build -b rpi_pico/rp2040/w /workspace/app --build-dir "${BUILD_DIR}" -t ram_report
else
    echo "=== Memory summary (run with --verbose for full per-symbol tree) ==="
    python3 /workspace/app/../scripts/elf_map.py "${BUILD_DIR}/zephyr/zephyr.elf"
fi
