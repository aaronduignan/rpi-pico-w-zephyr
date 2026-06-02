#!/bin/bash
# Fetch CYW43439 firmware blobs needed for WiFi and Bluetooth.
# Run inside the container: docker compose run zephyr ./scripts/fetch-blobs.sh
set -e

BLOB_DIR=/workspace/modules/hal/infineon/zephyr/blobs

WIFI_FW_DIR=${BLOB_DIR}/img/whd/resources/firmware/COMPONENT_43439
# WIFI_CLM_DIR=${BLOB_DIR}/img/whd/resources/clm/COMPONENT_43439/COMPONENT_CYW943439M2IPA1
WIFI_CLM_DIR=${BLOB_DIR}/img/whd/resources/clm/COMPONENT_43439/COMPONENT_MURATA-1YN
BT_FW_DIR=${BLOB_DIR}/img/bluetooth/firmware/COMPONENT_43439/COMPONENT_MURATA-1YN

mkdir -p "${WIFI_FW_DIR}" "${WIFI_CLM_DIR}" "${BT_FW_DIR}"

echo "Fetching CYW43439 WiFi firmware..."
wget -q https://github.com/Infineon/whd-expansion/raw/release-v1.1.0/WHD/COMPONENT_WIFI5/resources/firmware/COMPONENT_43439/43439A0.bin \
     -O "${WIFI_FW_DIR}/43439A0.bin"

echo "Fetching CYW43439 WiFi CLM blob..."
wget -q https://github.com/Infineon/wifi-resources/raw/release-v2.0.0/clm/COMPONENT_WIFI5/COMPONENT_43439/COMPONENT_CYW943439M2IPA1/43439A0.clm_blob \
     -O "${WIFI_CLM_DIR}/43439A0.clm_blob"

echo "Fetching CYW43439 Bluetooth firmware..."
wget -q https://raw.githubusercontent.com/Infineon/btstack-integration/release-v4.1.1/COMPONENT_HCI-UART/firmware/COMPONENT_43439/COMPONENT_MURATA-1YN/CYW4343A2_001.003.016.0031.0000_Generic_UART_37_4MHz_wlbga_BU_dl_signed.hcd \
     -O "${BT_FW_DIR}/bt_firmware.hcd"

echo "Blobs ready:"
echo "  ${WIFI_FW_DIR}/43439A0.bin"
echo "  ${WIFI_CLM_DIR}/43439A0.clm_blob"
echo "  ${BT_FW_DIR}/bt_firmware.hcd"
