# CYW43 Bluetooth Reference

A reference for understanding the CYW43439 Bluetooth implementation on the Raspberry Pi Pico W.

## External References

- [raspberrypi/pico-examples](https://github.com/raspberrypi/pico-examples) — `bluetooth/` directory has working BLE examples using the same driver
- [Zephyr issue #84570](https://github.com/zephyrproject-rtos/zephyr/issues/84570) — Pico W Bluetooth in Zephyr
- [Pico W Datasheet](https://datasheets.raspberrypi.com/picow/pico-w-datasheet.pdf)

## Architecture Diagrams

- [Hardware Block Diagram](../architecture/hw-block-diagram.md)
- [Software Stack](../architecture/sw-stack.md)
- [CYW43 Shared Bus](../architecture/cyw43-shared-bus.md)

---

## 1. GPIO Pinout

File: `workspace/modules/hal/rpi_pico/src/boards/include/boards/pico_w.h`

| GPIO | Name | Role |
|------|------|------|
| 23 | WL_REG_ON | Power enable for CYW43 |
| 24 | WL_DATA_OUT/IN/HOST_WAKE | Bidirectional SPI data **and** interrupt — shared pin |
| 25 | WL_CS | SPI chip select |
| 29 | WL_CLOCK | SPI clock |
| CYW43 GPIO 0 | — | Onboard LED |
| CYW43 GPIO 1 | — | SMPS power mode |
| CYW43 GPIO 2 | — | VBUS sense |

---

## 2. The Shared SPI Bus

Files:
- `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/cybt_shared_bus/cybt_shared_bus_driver.c`
- `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/cybt_shared_bus/cybt_shared_bus.c`

WiFi and BT share a **single PIO SPI bus**. WiFi must initialise first.

| Function | Purpose |
|----------|---------|
| `cybt_sharedbus_driver_init(cyw43_ll_t *)` | Hooks BT into the CYW43 low-level driver |
| `cybt_fw_download()` | Downloads BT firmware blob into CYW43 RAM at `0x19000000` |
| `cybt_set_host_ready()` | Signals readiness via `HOST_CTRL_REG (0x18000d6c)` |
| `cybt_toggle_bt_intr()` | Wakes BT firmware |
| `cybt_mem_read/write()` | Transfers data — 4-byte aligned, 1KB page boundaries |

Circular buffers manage traffic: **H2B** (Host→BT) and **B2H** (BT→Host).

> ⚠️ `CYBT_CORRUPTION_TEST` flag in the source documents a known risk of register corruption when WiFi and BT are both under heavy load simultaneously.

---

## 3. PIO SPI Implementation

Files:
- `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.c`
- `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.pio`

| Function | Purpose |
|----------|---------|
| `cyw43_spi_init()` | Claims DMA channels, configures GPIO pins into PIO mode |
| `start_spi_comms()` | Enables PIO functions, pulls CS low |

- Transfers in 32-bit units via hardware DMA
- Two PIO program variants for different clock/sampling configurations

---

## 4. BTstack Initialisation

File: `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/btstack_cyw43.c`

`btstack_cyw43_init()` sequence:
1. Init BTstack memory allocator
2. Init run loop with async context
3. Init HCI transport using CYW43 backend
4. Set up TLV flash storage for link keys
5. Configure BLE + classic device databases

BT device address = WiFi MAC + 1. See `btstack_chipset_cyw43.c`.

---

## 5. HCI Transport Layer

File: `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/btstack_hci_transport_cyw43.c`

| Function | Purpose |
|----------|---------|
| `hci_transport_cyw43_open()` | Calls `cyw43_bluetooth_hci_init()`, registers packet handler |
| `hci_transport_cyw43_send_packet()` | Prepends 4-byte CYW43 header and sends |
| `hci_transport_cyw43_process()` | Polls for incoming packets via `cyw43_bluetooth_hci_read()` |

Every HCI packet requires a **4-byte CYW43 header**: `[Len_B0, Len_B1, 0x00, HCI_Packet_Type]`

---

## 6. Interrupt Handling

File: `workspace/modules/hal/rpi_pico/src/rp2_common/pico_cyw43_driver/cyw43_driver.c`

- GPIO 24 (HOST_WAKE) fires a **level-high interrupt** when the CYW43 has data ready
- Work is dispatched via `async_context_t` (non-blocking)
- `cyw43_post_poll_hook()` re-enables the GPIO interrupt after each poll cycle

---

## 7. pico-examples Reference

Repository: [raspberrypi/pico-examples](https://github.com/raspberrypi/pico-examples)

| Example | What it shows |
|---------|--------------|
| `bt_ble_scan_connect/` | BLE scan + connect |
| `bt_ble_appearance_client/` | Read device appearance via GATT |
| `bt_gap_inquiry/` | BR/EDR classic device discovery |
| `bt_hid_keyboard_demo/` | BT classic HID profile |
