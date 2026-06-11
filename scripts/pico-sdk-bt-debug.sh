#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${SCRIPT_DIR}/build/pico-examples-picow-debug"
PICOTOOL_DEPS="${SCRIPT_DIR}/build/pico-examples-picow/_deps"
CURRENT_ELF="${BUILD_DIR}/current-pico-sdk-bt.elf"
CURRENT_BUILD_INFO="${CURRENT_ELF}.build-info"

example="${1:-gatt_counter}"

case "${example}" in
  temp_sensor)
    target="picow_ble_temp_sensor"
    elf="${BUILD_DIR}/pico_w/bt/standalone/server/picow_ble_temp_sensor.elf"
    ;;
  gatt_counter)
    target="picow_bt_example_gatt_counter_background"
    elf="${BUILD_DIR}/pico_w/bt/gatt_counter/picow_bt_example_gatt_counter_background.elf"
    ;;
  gap_le_advertisements)
    target="picow_bt_example_gap_le_advertisements_background"
    elf="${BUILD_DIR}/pico_w/bt/gap_le_advertisements/picow_bt_example_gap_le_advertisements_background.elf"
    ;;
  nordic_spp_counter)
    target="picow_bt_example_nordic_spp_le_counter_background"
    elf="${BUILD_DIR}/pico_w/bt/nordic_spp_le_counter/picow_bt_example_nordic_spp_le_counter_background.elf"
    ;;
  *)
    echo "Unknown Pico SDK BT example: ${example}" >&2
    echo "Known examples: temp_sensor, gatt_counter, gap_le_advertisements, nordic_spp_counter" >&2
    exit 2
    ;;
esac

sdk_branch=$(git -C "${SCRIPT_DIR}/pico-sdk" branch --show-current 2>/dev/null || true)
sdk_commit=$(git -C "${SCRIPT_DIR}/pico-sdk" rev-parse --short HEAD 2>/dev/null || true)
sdk_branch=${sdk_branch:-detached}
sdk_commit=${sdk_commit:-unknown}

echo "Building ${example} from pico-sdk ${sdk_branch} ${sdk_commit}"

cmake -S "${SCRIPT_DIR}/pico-examples" -B "${BUILD_DIR}" \
  -DPICO_SDK_PATH="${SCRIPT_DIR}/pico-sdk" \
  -DPICO_BOARD=pico_w \
  -DBTSTACK_EXAMPLE_TYPE=background \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPICOTOOL_FETCH_FROM_GIT_PATH="${PICOTOOL_DEPS}"

cmake --build "${BUILD_DIR}" --target "${target}"

cp "${elf}" "${CURRENT_ELF}"
{
  echo "example=${example}"
  echo "target=${target}"
  echo "sdk_branch=${sdk_branch}"
  echo "sdk_commit=${sdk_commit}"
  echo "elf=${elf}"
} > "${CURRENT_BUILD_INFO}"
echo "Prepared ${example}: ${CURRENT_ELF}"
echo "Build info: ${CURRENT_BUILD_INFO}"
