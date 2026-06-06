# ble_logger — Raspberry Pi Pico W (RP2040)

Scans for nearby BLE devices, lets you pick one by number, then logs every
advertisement it sends — raw AD fields as hex — to the UART.

## Usage

Connect a serial terminal at 115200 baud and use the `ble` shell commands.

**Discover devices** (10 s passive scan):
```
pico:~$ ble scan
Scanning for 10 s...
[ 1] 11:22:33:44:55:66 (public)  RSSI -81 dBm
[ 2] AA:BB:CC:DD:EE:FF (random)  RSSI -72 dBm
Discovery complete — 2 device(s). Use 'ble log <N>' to start logging.
```

**Log a device** (passive scan, continuous):
```
pico:~$ ble log 1
Logging 11:22:33:44:55:66 (public) — use 'ble stop' to end
[0:00:15.432] 11:22:33:44:55:66 (public)  RSSI -80 dBm
  [0x01] 06
  [0x09] 4d 79 20 44 65 76 69 63 65
  [0xff] 4c 00 10 05 01 ...
```

Each `[0xNN]` line is one AD field: `NN` is the AD type, the bytes following
are the payload in hex (truncated at 40 bytes with `...`).

**Stop**:
```
pico:~$ ble stop
```

## Notes

- Both phases use passive scan. Active scanning (`CONFIG_BT_CENTRAL`) causes
  HCI `LE Set Scan Enable` timeouts on the CYW43439's shared SPI bus — the
  BT and WiFi cores share the bus without arbitration, and the controller
  goes unresponsive under the extra load of sending SCAN_REQ PDUs. Device
  names are therefore unavailable.
- For logging, choose devices with a `public` address when possible. Devices
  using `random` or private resolvable addresses can show up during discovery,
  but their address may change or resolve differently, so `ble log <N>` may
  start scanning and never match any advertisement data for that entry.
- Run `ble scan` again at any time to refresh the device list; the previous
  list is cleared.
- Up to 32 devices are tracked per scan session.
