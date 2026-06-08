# Pico SDK CYW43 BLE Delay Investigation

This directory backs up the local Pico SDK BLE bring-up investigation from
June 2026. The SDK and CYW43 driver are nested Git checkouts, so these patches
are stored in the parent repo branch as an extra backup.

## Clean candidate fix

- `0001-Use-non-scheduling-CYW43-driver-delays.patch`
  - Source repo: `pico-sdk`
  - Source branch: `fix-cyw43-non-scheduling-delay`
  - Source commit: `9551cae6 Use non-scheduling CYW43 driver delays`
  - Change: implement `cyw43_delay_ms()` and `cyw43_delay_us()` with
    `busy_wait_ms()` / `busy_wait_us()` instead of `async_context_wait_until()`.

## Diagnostic SDK series

- `0001-Trace-and-bypass-CYW43-delay-hang.patch`
- `0002-Use-non-scheduling-CYW43-driver-delays.patch`
  - Source repo: `pico-sdk`
  - Source branch: `debug-cyw43-delay-root-cause`
  - Source commits:
    - `f7eced11 Trace and bypass CYW43 delay hang`
    - `eaf4d201 Use non-scheduling CYW43 driver delays`

## Diagnostic CYW43 driver patch

- `0001-Instrument-CYW43-bring-up-delay-hang.patch`
  - Source repo: `pico-sdk/lib/cyw43-driver`
  - Source branch: `debug-cyw43-delay-root-cause`
  - Source commit: `47e7be1 Instrument CYW43 bring-up delay hang`

## Current finding

The Pico SDK BLE examples reached `hci_power_control(HCI_POWER_ON)` and then
stopped during CYW43 bring-up. Tracing showed the stop after `WL_REG_ON` was
pulled low, at the first `cyw43_delay_ms(20)` in `cyw43_ensure_up()`.

The original SDK 2.2.0 delay hook routed through `async_context_wait_until()`.
Changing the CYW43 HAL delay hooks to busy waits allowed the GATT counter BLE
example to advertise.

This supports the hypothesis that CYW43 chip reset/firmware bring-up should
not use the SDK sleep/alarm path for its low-level HAL delay. The exact
upstream fix should still be validated against all async backends.
