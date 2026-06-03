# RPi Pico W — Zephyr Development Environment

Containerised Zephyr RTOS build environment for the Raspberry Pi Pico W, with SWD flashing via the Raspberry Pi Debug Probe.

## Supported Platforms

| Platform | Build | Flash | Serial |
|---|---|---|---|
| macOS Intel | ✅ | ✅ | ✅ |
| macOS Apple Silicon | ⚠️ Slow (Rosetta emulation) | ✅ | ✅ |
| Linux (Ubuntu/Debian) | ✅ | ✅ | ✅ |
| Windows (WSL2) | ⚠️ Partial | ⚠️ Complex | ⚠️ PuTTY |

## Hardware Required

- Raspberry Pi Pico W (target)
- [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/)
- USB-C cable (debug probe)
- USB-Micro or USB-C cable (Pico W power, optional)

## Wiring

Refer to the [official Debug Probe documentation](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) for the SWD and UART pinout.

## Prerequisites

### macOS (Intel & Apple Silicon)

```bash
brew install openocd
```

[Install Docker Desktop for Mac](https://www.docker.com/products/docker-desktop/)

### Linux (Ubuntu/Debian)

```bash
sudo apt install openocd
```

[Install Docker Engine](https://docs.docker.com/engine/install/)

Add yourself to the `dialout` group for serial access:

```bash
sudo usermod -aG dialout $USER
```

### Windows

Use WSL2 with Docker Desktop. Flash and serial steps require additional setup — see the [OpenOCD Windows guide](https://openocd.org).

---

## Setup (one-time)

> **Warning:** The workspace initialization (`init-workspace.sh`) downloads the entire Zephyr source tree and all module dependencies (~5 GB). It will take a long time and peg your CPU. Run it when you don't need your machine for anything else and are due a coffee.

**1. Build the Docker image** — downloads the Zephyr SDK ARM toolchain and host tools. Takes ~5-10 min.

```bash
docker compose build
```

**2. Initialize the Zephyr workspace** — clones Zephyr and all modules into `workspace/`. Uses `--depth=1` shallow clones (latest commit only, no history) to keep the download manageable. Still takes 20-40 min and ~3 GB on first run. Only needed once per machine; `workspace/` is gitignored.

```bash
docker compose run zephyr ./scripts/init-workspace.sh
```

---

## Usage

**Build**

```bash
docker compose run zephyr ./scripts/build.sh
```

Output: `build/zephyr/zephyr.elf`

To build a specific sample (default is `ble_advertiser`):

```bash
SAMPLE=ble_advertiser docker compose run zephyr ./scripts/build.sh
```

**Flash** (run on host, not in Docker)

```bash
./scripts/flash.sh
```

**Serial monitor**

macOS:
```bash
ls /dev/tty.usbmodem*
screen /dev/tty.usbmodem* 115200
```

Linux:
```bash
ls /dev/ttyACM*
screen /dev/ttyACM0 115200
```

Exit `screen` with `Ctrl-A` then `K`, then `y` to confirm.

---

## Project Structure

```
.
├── Dockerfile                  # Zephyr SDK build environment
├── docker-compose.yml
├── drivers/
│   └── cyw43/bt/               # CYW43439 BT HCI shared-bus driver
├── dts/bindings/bluetooth/     # Out-of-tree DT binding for the BT HCI node
├── boards/                     # Board DTS overlays (shared across samples)
├── firmware/                   # CYW43439 BT firmware blob and NVRAM
├── samples/
│   └── ble_advertiser/         # Non-connectable BLE beacon sample
│       ├── CMakeLists.txt
│       ├── prj.conf
│       └── src/main.c
├── openocd/
│   └── picoprobe.cfg           # OpenOCD config for Debug Probe
└── scripts/
    ├── init-workspace.sh       # One-time west workspace setup
    ├── build.sh                # Build firmware (run in Docker)
    └── flash.sh                # Flash via OpenOCD (run on host)
```

## Zephyr Version

- Zephyr: `v4.4.0`
- Zephyr SDK: `1.0.1`
- Board: `rpi_pico/rp2040/w`
