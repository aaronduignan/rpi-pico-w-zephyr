# Bluetooth Bring-up on Raspberry Pi Pico W (Zephyr)

## Status

Proven working as of this branch:
- WiFi firmware (v7.95.61 with `btsdio`) loads via WHD
- BT firmware patch downloads via WHD backplane
- WLAN shared-buffer base (`0x0006861c`) is published by BT firmware
- Ring buffers initialised
- HCI `READ_LOCAL_VERSION` command sent and response received
- BT chip confirmed: Cypress, HCI v11 (Bluetooth 5.2)

Remaining work: wire up the Zephyr HCI transport layer (RX thread, `send`/`recv`).

---

## Hardware Architecture

The Pico W has one CYW43439 chip that contains both a WiFi ARM core and a BT ARM
core. There is no dedicated BT UART — BT packets go over the same PIO SPI bus
that WiFi uses, multiplexed via the BTSDIO shared-bus protocol.

```
┌──────────────────────────────────────────────────────────────────┐
│  Raspberry Pi Pico W                                             │
│                                                                  │
│  ┌──────────────────────────────┐                               │
│  │  RP2040                      │                               │
│  │                              │                               │
│  │  ┌──────────────┐            │    PIO SPI (gSPI)            │
│  │  │  Application │            │    ─────────────────────────→ │
│  │  │  (Zephyr)    │            │    CLK / MOSI / MISO / CS    │
│  │  └──────┬───────┘            │                               │
│  │         │ Zephyr BT HCI API  │                               │
│  │  ┌──────▼───────────────┐    │                               │
│  │  │  hci_cyw43_shared_bus│    │                               │
│  │  │  (our driver)        │    │                               │
│  │  └──────┬───────────────┘    │                               │
│  │         │ WHD backplane API  │                               │
│  │  ┌──────▼───────────────┐    │                               │
│  │  │  WHD (Infineon WiFi  │    │                               │
│  │  │  Host Driver)        │────┤                               │
│  │  └──────────────────────┘    │                               │
│  └──────────────────────────────┘                               │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  CYW43439                                                 │  │
│  │                                                           │  │
│  │  ┌────────────────────────┐   ┌──────────────────────┐  │  │
│  │  │  WLAN ARM core         │   │  BT ARM core         │  │  │
│  │  │                        │   │                      │  │  │
│  │  │  Firmware: v7.95.61    │   │  Firmware: ROM +     │  │  │
│  │  │  Features: btsdio,     │   │  SDIO patch          │  │  │
│  │  │  btcx, swdiv, ...      │   │  (6970 byte patch)   │  │  │
│  │  │                        │   │                      │  │  │
│  │  │  Manages BTSDIO        │   │  Publishes ring buf  │  │  │
│  │  │  mailbox protocol      │   │  base addr after     │  │  │
│  │  │  with BT core          │   │  firmware ready      │  │  │
│  │  └──────────┬─────────────┘   └──────────┬───────────┘  │  │
│  │             │                             │              │  │
│  │             └──────── shared RAM ─────────┘              │  │
│  │                    (ring buffers at 0x0006861c)           │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

---

## Firmware Components

Three firmware blobs are baked into the flash image at build time alongside the
application code. All blobs land in the `.rodata` section (ELF address
`0x100240b8`, 261 KB total) which is where the bulk of flash is spent.

```
Flash layout (2 MB total, measured from ELF):
┌──────────────────────────────────────────┐  0x10000000
│  Boot block                  256 B       │
├──────────────────────────────────────────┤  0x10000100
│  Code (.text)               142 KB       │
│  kernel, WHD, SPI driver,               │
│  BT HCI driver, shell, logging          │
├──────────────────────────────────────────┤  0x100240b8  ← .rodata start
│  WiFi firmware blob         226 KB       │  0x100240d8
│  wb43439A0_7_95_49_00_bluetooth.bin      │
│  v7.95.61  —  has 'btsdio'              │
│                                          │
│  Key: the WLAN core implements the       │
│  BTSDIO mailbox protocol. The stock      │
│  AIROC v7.95.88 lacks this entirely.     │
├──────────────────────────────────────────┤  0x1005da10
│  NVRAM                        740 B      │
│  picow_nvram.txt                         │
│  Critical: muxenab=0x100                 │
│  Without this the firmware stalls        │
│  during antenna GPIO mux init.           │
├──────────────────────────────────────────┤  0x10063afc
│  BT firmware patch          6.8 KB       │
│  cyw43_btfw_43439.h                      │
│  SDIO format, v1.3.16.65                 │
├──────────────────────────────────────────┤
│  CLM blob + misc rodata      ~30 KB      │
├──────────────────────────────────────────┤  0x100663c0  ← end of content
│  (free)                    1,642 KB      │
└──────────────────────────────────────────┘  0x101fffff
```

The `.rodata` section is the first place to look if flash gets tight — the
WiFi firmware alone is 226 KB. Run `./scripts/memory_report.sh` at any time
to see current usage.

---

## BTSDIO Shared-Bus Initialisation Sequence

This is the sequence our driver (`hci_cyw43_shared_bus.c`) performs:

```
RP2040 (Zephyr)                  CYW43439 WLAN core         BT core
     │                                  │                       │
     │  1. WHD loads WiFi firmware      │                       │
     │     (wb43439A0...bluetooth.bin)  │                       │
     │─────────────────────────────────→│                       │
     │                                  │  2. WLAN core boots   │
     │                                  │     BT core boots     │
     │                                  │     from ROM          │
     │                                  │──────────────────────→│
     │                                  │                       │
     │  3. whd_bus_bt_attach()          │                       │
     │     (registers BT on shared bus) │                       │
     │─────────────────────────────────→│                       │
     │                                  │                       │
     │  4. Download BT patch            │                       │
     │     6970 bytes via WHD backplane │  5. Writes to BT RAM  │
     │     API to 0x19000000+           │  via backplane window │
     │─────────────────────────────────→│──────────────────────→│
     │                                  │                       │
     │  6. Write BT2WLAN_PWRUP_WAKE     │  BT patch activates   │
     │     to 0x19640894                │──────────────────────→│
     │─────────────────────────────────→│                       │
     │                                  │                       │
     │  7. Poll BT_CTRL_REG (0x18000c7c)│                       │
     │     Wait for FW_RDY bit (bit 24) │                       │
     │       ←─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│←──────────────────────│
     │                                  │                       │
     │  8. Read WLAN_RAM_BASE_REG       │  BT core has written  │
     │     (0x18000d68) → 0x0006861c    │  ring buf base here   │
     │←─────────────────────────────────│←──────────────────────│
     │                                  │                       │
     │  9. Set up ring buffer pointers  │                       │
     │     h2b_buf = base + 0x0000      │                       │
     │     b2h_buf = base + 0x1000      │                       │
     │     h2b_in/out = base + 0x2000   │                       │
     │     b2h_in/out = base + 0x2008   │                       │
     │                                  │                       │
     │ 10. Wake BT: set WAKE_BT bit     │                       │
     │     in HOST_CTRL_REG (0x18000d6c)│──────────────────────→│
     │─────────────────────────────────→│                       │
     │                                  │                       │
     │ 11. Set host ready: SW_RDY bit   │                       │
     │     in HOST_CTRL_REG             │                       │
     │─────────────────────────────────→│                       │
     │                                  │                       │
     │ 12. Toggle DATA_VALID bit        │  BT stack running,    │
     │     in HOST_CTRL_REG             │  ring buffers live    │
     │─────────────────────────────────→│──────────────────────→│
     │                                  │                       │
     │ 13. HCI command → ring write     │                       │
     │     READ_LOCAL_VERSION (0x1001)  │                       │
     │─────────────────────────────────→│──────────────────────→│
     │                                  │                       │
     │ 14. HCI event ← ring read        │                       │
     │     CMD_COMPLETE, status=0x00    │                       │
     │←─────────────────────────────────│←──────────────────────│
```

---

## Ring Buffer Layout

The BTSDIO protocol uses two 4 KB circular buffers in shared RAM:

```
  WLAN_RAM_BASE = 0x0006861c  (published by BT firmware in WLAN_RAM_BASE_REG)

  Offset 0x0000  ┌─────────────────────────────┐
                 │  Host-to-BT buffer (4 KB)   │  h2b_buf
  Offset 0x1000  ├─────────────────────────────┤
                 │  BT-to-Host buffer (4 KB)   │  b2h_buf
  Offset 0x2000  ├─────────────────────────────┤
                 │  h2b_in  (uint32)           │  host write pointer
  Offset 0x2004  │  h2b_out (uint32)           │  BT read pointer
  Offset 0x2008  │  b2h_in  (uint32)           │  BT write pointer
  Offset 0x200c  │  b2h_out (uint32)           │  host read pointer
                 └─────────────────────────────┘

  HCI packet format over ring:
  ┌────────────────┬───────────────────────────┐
  │ 3B: length     │ 1B: HCI packet type (0x01 │ payload ...
  │ (little-endian)│ cmd / 0x02 ACL / 0x04 evt)│
  └────────────────┴───────────────────────────┘
```

---

## Why the Stock AIROC Firmware Doesn't Work

The AIROC `43439A0.bin` (v7.95.88) from `whd-expansion/release-v1.1.0` is a
WiFi-only firmware built without the `btsdio` feature. It has no BT mailbox
protocol implementation in the WLAN core, so `WLAN_RAM_BASE_REG` is always 0.

The stock AIROC BT blob (`bt_firmware.hcd`) is in **HCI UART format** —
intended for boards that wire the CYW43439's BT UART pins to the host. The
Pico W has no such wiring; BT must go via the shared SPI bus.

| | AIROC default | This branch |
|---|---|---|
| WiFi firmware | `43439A0.bin` v7.95.88 | `wb43439A0...bluetooth.bin` v7.95.61 |
| `btsdio` in WLAN core | No | Yes |
| BT transport | UART (not wired on Pico W) | BTSDIO shared bus |
| BT firmware format | `.hcd` (HCI commands over UART) | Binary SDIO patch |
| `WLAN_RAM_BASE_REG` | Always 0 | 0x0006861c |

---

## Key Discovery: NVRAM

The pico-sdk firmware (v7.95.61) stalls during boot if loaded with the generic
Murata-1YN NVRAM (`cyfmac43439-1YN.txt`) that AIROC ships. The firmware never
grants the HT (high-speed) clock to WHD, causing a 2-second timeout.

The root cause is a missing `muxenab=0x100` in the AIROC NVRAM. This controls
the antenna GPIO mux on the Pico W PCB. Without it the firmware spins in RF
init and never completes boot.

Fix: use the NVRAM from the pico-sdk's `cyw43-driver` repo
(`firmware/wifi_nvram_43439.h`). It is saved as `app/firmware/picow_nvram.txt`.

---

## Memory Usage

Measured from `app/build/zephyr/zephyr.elf` on this branch.
Re-run anytime with `docker compose run --rm zephyr ./scripts/memory_report.sh`.

```
FLASH  [#######.................................]  405.6 KB / 2.0 MB  (19.8%)

  0x10000000  .boot2                      256 B
  0x10000100  rom_start / vectors         168 B
  0x100001a8  .text (code)            142 KB     kernel, drivers, BT stack
  0x100240b8  .rodata                 261 KB     ← blobs live here
    0x100240d8  wifi_firmware_image_data  226 KB   WiFi fw v7.95.61
    0x1005da10  wifi_nvram_image_data       740 B   NVRAM
    0x10063afc  cyw43_btfw_43439          6.8 KB   BT patch
                CLM blob + misc          ~30 KB
  0x100663c0  (end)

RAM    [##############..........................]   94.2 KB / 264 KB  (35.7%)

  0x20000000  .data / init globals        2.5 KB
  0x20000d80  .bss                        8.9 KB
  0x20003138  .noinit (large buffers)    81.9 KB
    net_buf_data_airoc_pool             31.2 KB   WHD WiFi buffer pool
    kheap__system_heap                  16.1 KB   Zephyr heap
    net_buf_data_tx/rx_bufs              9.0 KB   TCP/IP buffers
    thread stacks (main, WiFi, shell…)  ~13 KB
```

---

## What Remains (TODO)

- [ ] RX polling thread — call `data->recv()` with incoming HCI events
- [ ] `cyw43_bt_send()` — marshal `net_buf` to ring write + wake BT
- [ ] Remove `-ENOSYS` returns so `bt_enable()` completes
- [ ] `prj.conf` additions for full BT stack (`CONFIG_BT_HCI_DRIVER=y` etc.)
- [ ] Interrupt-driven RX (currently only probe path, no live polling)
