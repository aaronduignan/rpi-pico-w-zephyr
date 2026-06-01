#!/bin/bash
# Run once to initialise the Zephyr west workspace inside the container.
set -e

ZEPHYR_VERSION=${ZEPHYR_VERSION:-v3.7.0}

echo "Initialising Zephyr workspace (${ZEPHYR_VERSION})..."
west init -m https://github.com/zephyrproject-rtos/zephyr --mr "${ZEPHYR_VERSION}" /workspace
cd /workspace
west update
west zephyr-export
pip3 install -r zephyr/scripts/requirements.txt
echo "Workspace ready."
