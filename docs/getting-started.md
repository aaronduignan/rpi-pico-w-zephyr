# Getting Started

A curated set of references for developing on the Raspberry Pi Pico W with Zephyr RTOS.

---

## Hardware

### Raspberry Pi Pico W
- [Pico W Datasheet](https://datasheets.raspberrypi.com/picow/pico-w-datasheet.pdf) — pinout, electrical specs, CYW43439 interface
- [Pico W Schematic](https://datasheets.raspberrypi.com/picow/pico-w-schematic.pdf) — full schematic including CYW43 wiring
- [Getting Started with Pico](https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf) — official getting started guide

### RP2040
- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — full chip reference including PIO, DMA, and peripherals
- [RP2040 Hardware Design Guide](https://datasheets.raspberrypi.com/rp2040/hardware-design-with-rp2040.pdf)

### CYW43439 (WiFi + Bluetooth)
- [Infineon CYW43439 Product Page](https://www.infineon.com/cms/en/product/wireless-connectivity/airoc-wi-fi-plus-bluetooth-combos/wi-fi-4-802.11n/cyw43439/) — datasheet and reference manual

### Debug Probe
- [Raspberry Pi Debug Probe](https://www.raspberrypi.com/products/debug-probe/) — product page
- [Debug Probe Documentation](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) — pinout, setup, and usage

---

## Zephyr RTOS

### Official Docs
- [Zephyr Documentation](https://docs.zephyrproject.org/latest/) — full reference
- [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) — environment setup
- [West (build tool)](https://docs.zephyrproject.org/latest/develop/west/index.html) — workspace and build management
- [Devicetree Guide](https://docs.zephyrproject.org/latest/build/dts/index.html) — DTS/DTSI/overlay reference
- [Kconfig Reference](https://docs.zephyrproject.org/latest/build/kconfig/index.html) — configuration system
- [Bluetooth Stack](https://docs.zephyrproject.org/latest/connectivity/bluetooth/index.html) — Zephyr BT/BLE API

### Board Support
- [Raspberry Pi Pico board docs](https://docs.zephyrproject.org/latest/boards/raspberrypi/rpi_pico/doc/index.html) — Zephyr board page for Pico / Pico W

### Repositories
- [zephyrproject-rtos/zephyr](https://github.com/zephyrproject-rtos/zephyr) — Zephyr RTOS source
- [zephyrproject-rtos/sdk-ng](https://github.com/zephyrproject-rtos/sdk-ng) — Zephyr SDK releases

---

## pico-sdk (Reference Implementation)

- [raspberrypi/pico-sdk](https://github.com/raspberrypi/pico-sdk) — official Pico SDK
- [raspberrypi/pico-examples](https://github.com/raspberrypi/pico-examples) — examples including full Bluetooth demos
- [pico-sdk Doxygen](https://raspberrypi.github.io/pico-sdk-doxygen/) — API reference

---

## Bluetooth

- [Bluetooth Core Specification 5.2](https://www.bluetooth.com/specifications/specs/) — search for "Core Specification 5.2" on the Bluetooth SIG specs page (free, registration required)
- [BTstack Documentation](https://bluekitchen-gmbh.com/btstack/) — the BT stack used by pico-sdk
- [Zephyr BT API](https://docs.zephyrproject.org/latest/connectivity/bluetooth/api/index.html)

---

## CYW43439 Firmware Blobs

The CYW43439 has no onboard flash — the RP2040 uploads proprietary Infineon firmware to it at every boot. These binary blobs cannot be open-sourced and must be fetched separately.

| Blob | Purpose |
|------|---------|
| `43439A0.bin` | WiFi firmware — uploaded to CYW43 RAM at boot |
| `43439A0.clm_blob` | Country Localization Module — regional WiFi regulatory limits |
| `bt_firmware.hcd` | BT firmware patch — uploaded via HCI transport before BT can be used |

**Fetch just the CYW43439 blobs** (fast, included in workspace init):

```bash
docker compose run zephyr ./scripts/fetch-blobs.sh
```

**Fetch all hal_infineon blobs** (slow — downloads firmware for every Infineon chip):

```bash
docker compose run zephyr bash -c "cd /workspace && west blobs fetch hal_infineon"
```

Blobs land in `workspace/modules/hal/infineon/zephyr/blobs/` and are baked into the ELF at build time. They are excluded from git via `.gitignore` on the `workspace/` directory.

---

## This Repo

- [Hardware Block Diagram](architecture/hw-block-diagram.md)
- [Software Stack](architecture/sw-stack.md)
- [CYW43 Shared Bus](architecture/cyw43-shared-bus.md)
- [CYW43 Bluetooth Reference](references/cyw43-bluetooth.md)
