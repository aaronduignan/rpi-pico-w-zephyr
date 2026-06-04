#!/bin/bash
# Ensure Docker is running before attempting a build.
set -e

if docker info > /dev/null 2>&1; then
    exit 0
fi

case "$(uname)" in
    Darwin)
        echo "Starting Docker Desktop..."
        open -a Docker
        ;;
    Linux)
        echo "Starting Docker service..."
        sudo systemctl start docker
        ;;
    *)
        echo "Unsupported platform: $(uname)" >&2
        exit 1
        ;;
esac

echo "Waiting for Docker..."
until docker info > /dev/null 2>&1; do
    sleep 1
done
echo "Docker ready."
