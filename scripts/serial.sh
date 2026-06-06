#!/bin/bash
# Requires screen >= 4.6 for --log support (-Logfile flag).
# macOS ships screen 4.00 (2006) — install a modern version first:
#   brew install screen
set -e

LOG=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --log) LOG=true ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

# Detect port — macOS first, then Linux
PORT=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
    PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1)
fi

if [ -z "$PORT" ]; then
    echo "Error: no serial device found. Is the debug probe plugged in?" >&2
    exit 1
fi

echo "Connecting to $PORT at 115200 baud. Exit with Ctrl-A then K, then y."

if $LOG; then
    mkdir -p logs
    LOGFILE="logs/$(date '+%Y%m%d-%H%M').log"
    echo "Logging to $LOGFILE"
    screen -L -Logfile "$LOGFILE" "$PORT" 115200
else
    screen "$PORT" 115200
fi
