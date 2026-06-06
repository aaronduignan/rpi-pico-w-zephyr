# CYW43 Shared Bus Architecture

The CYW43439 WiFi and Bluetooth cores share a single PIO SPI bus. Understanding this is key to implementing Bluetooth in Zephyr.

## Bus Arbitration

```mermaid
sequenceDiagram
    participant Host as RP2040 Host
    participant Bus as PIO SPI Bus
    participant WiFi as CYW43 WiFi Core
    participant BT as CYW43 BT Core

    Note over Host,BT: Initialisation
    Host->>Bus: cyw43_spi_init() - claim PIO + DMA
    Host->>WiFi: Power on via GPIO23 (WL_REG_ON)
    Host->>WiFi: WiFi firmware download
    Host->>BT: cybt_sharedbus_driver_init()
    Host->>BT: cybt_fw_download() → RAM @ 0x19000000
    Host->>BT: cybt_set_host_ready() → HOST_CTRL_REG 0x18000d6c

    Note over Host,BT: Normal Operation
    Host->>Bus: start_spi_comms() - CS low
    Host->>BT: cybt_toggle_bt_intr() - wake BT
    BT-->>Host: GPIO24 (HOST_WAKE) interrupt
    Host->>Bus: DMA transfer (32-bit units, 4-byte aligned)
    Bus-->>Host: B2H circular buffer (BT→Host)
    Host->>Bus: H2B circular buffer (Host→BT)
    Host->>Bus: end_spi_comms() - CS high

    Note over Host,BT: ⚠️ Contention Risk
    Host->>WiFi: WiFi TX (high load)
    Host->>BT: BT TX concurrent
    Note over Bus: Register corruption possible
    Note over Bus: CYBT_CORRUPTION_TEST flag
```

## HCI Packet Flow

```mermaid
sequenceDiagram
    participant App as Application
    participant ZBT as Zephyr BT Host
    participant HCI as HCI Transport
    participant Bus as Shared Bus

    App->>ZBT: bt_le_scan_start()
    ZBT->>HCI: HCI LE Set Scan Parameters
    HCI->>HCI: Prepend 4-byte CYW43 header
    Note over HCI: [Len_B0, Len_B1, 0x00, HCI_CMD]
    HCI->>Bus: hci_transport_cyw43_send_packet()
    Bus-->>HCI: Scan event via cyw43_bluetooth_hci_read()
    HCI-->>ZBT: bt_addr + RSSI + AD data
    ZBT-->>App: device_found() callback
```

## Memory Map

```mermaid
graph TD
    subgraph CYW43 Memory
        REG1["BT_CTRL_REG\n0x18000c7c"]
        REG2["HOST_CTRL_REG\n0x18000d6c"]
        REG3["WLAN_RAM_BASE_REG\n0x18000d68"]
        FW["BT Firmware\n0x19000000"]
    end

    subgraph Buffers
        H2B["H2B Circular Buffer\n(Host → BT)"]
        B2H["B2H Circular Buffer\n(BT → Host)"]
    end

    HOST["RP2040 Host"] -->|"cybt_reg_write()"| REG1
    HOST -->|"cybt_set_host_ready()"| REG2
    HOST -->|"cybt_fw_download()\n1KB page aligned"| FW
    HOST <-->|"cybt_mem_read/write()"| H2B
    HOST <-->|"cybt_mem_read/write()"| B2H
```
