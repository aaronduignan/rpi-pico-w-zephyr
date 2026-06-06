# Getting Started

A curated set of references for developing on the Raspberry Pi Pico W with Zephyr RTOS.

---

## Hardware Quick Reference — Pico W (RP2040)

> Full interactive pinout: **[pinout.xyz/pinout/raspberry_pi_pico_w](https://pinout.xyz/pinout/raspberry_pi_pico_w)**
> Official PDF: **[datasheets.raspberrypi.com/picow/pico-w-pinout.pdf](https://datasheets.raspberrypi.com/picow/pico-w-pinout.pdf)**

### Memory

| | |
|---|---|
| Flash | 2 MB (XIP, external) |
| SRAM | 264 KB (6 banks) |
| Processor | Dual-core Arm Cortex-M0+ @ up to 133 MHz |

### User GPIO

GPIO0–GPIO22 and GPIO26–GPIO28 are available on the 40-pin header (26 pins).

| GPIO | Reserved for |
|---|---|
| GPIO23 | SMPS power-save control — do not use |
| GPIO24 | CYW43439 SPI data/IRQ — do not use |
| GPIO25 | CYW43439 chip-select / on — do not use (LED on regular Pico) |
| GPIO29 | VSYS/3 ADC measurement on Pico W — avoid as general GPIO |

### Peripherals

| Peripheral | Count | Default pins (Zephyr) | Notes |
|---|---|---|---|
| **UART** | 2 | UART0: GP0 (TX), GP1 (RX) | UART0 is the Zephyr console by default |
| | | UART1: GP4 (TX), GP5 (RX) | All UART pins are remappable |
| **SPI** | 2 | SPI0: GP2 (SCK), GP3 (MOSI), GP4 (MISO) | SPI1 is used internally by the CYW43439 — not user-accessible |
| **I2C** | 2 | I2C0: GP4 (SDA), GP5 (SCL) | All I2C pins are remappable |
| | | I2C1: GP2 (SDA), GP3 (SCL) | |
| **PWM** | 8 slices × 2 ch | Any GPIO | Each GPIO pair shares a slice (e.g. GP0/GP1 = slice 0 A/B) |
| **ADC** | 4 ch (12-bit) | GP26 = ADC0, GP27 = ADC1, GP28 = ADC2 | ADC3 (GP29) reserved for VSYS on Pico W; internal ch4 = temperature sensor |
| **PIO** | 2 blocks × 4 SM | Any GPIO | 8 state machines total; used for custom protocols (e.g. CYW43439 SPI) |
| **USB** | 1 | — | Native USB 1.1 host/device |

### CYW43439 (WiFi + Bluetooth)

The onboard LED is on **WL_GPIO0** of the CYW43439 — not on the RP2040. It requires the WiFi driver to be initialised and is controlled via the `cyw43_gpio` driver, not a standard GPIO. See [CYW43 Shared Bus](architecture/cyw43-shared-bus.md) for the shared SPI bus constraints.

---

## Hardware

### Raspberry Pi Pico W (RP2040)
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

The CYW43439 has no onboard flash — the RP2040 uploads proprietary firmware to it at every boot. This repo keeps the Pico SDK WiFi firmware variant and matching NVRAM in `firmware/`; the CLM regulatory blob is fetched into the Zephyr workspace volume.

| Blob | Purpose |
|------|---------|
| `firmware/wb43439A0_7_95_49_00_bluetooth.bin` | Pico SDK WiFi firmware variant with `btsdio`; required for the shared WiFi/BLE bus |
| `firmware/picow_nvram.txt` | Pico W NVRAM with board-specific RF/GPIO settings |
| `/workspace/modules/hal/infineon/.../43439A0.clm_blob` | Country Localization Module — regional WiFi regulatory limits |
| `drivers/cyw43/bt/cyw43_btfw_43439.h` | BT SDIO firmware patch embedded by the driver |

**Fetch the Zephyr-managed CYW43439 blobs** (included in workspace init):

```bash
docker compose run zephyr ./scripts/fetch-blobs.sh
```

**Fetch all hal_infineon blobs** (slow — downloads firmware for every Infineon chip):

```bash
docker compose run zephyr bash -c "cd /workspace && west blobs fetch hal_infineon"
```

Zephyr-managed blobs land in `/workspace/modules/hal/infineon/zephyr/blobs/` inside the Docker `zephyr-workspace` volume and are baked into the ELF at build time. The volume is outside git.

---

## This Repo

- [Hardware Block Diagram](architecture/hw-block-diagram.md)
- [Software Stack](architecture/sw-stack.md)
- [CYW43 Shared Bus](architecture/cyw43-shared-bus.md)
- [CYW43 Bluetooth Reference](references/cyw43-bluetooth.md)
