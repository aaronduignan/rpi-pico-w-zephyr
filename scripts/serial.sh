#!/bin/bash
set -e

PORT=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)

if [ -z "$PORT" ]; then
    echo "Error: no usbmodem device found. Is the debug probe plugged in?"
    exit 1
fi

echo "Connecting to $PORT at 115200 baud. Exit with Ctrl-A then K, then y."
screen "$PORT" 115200
