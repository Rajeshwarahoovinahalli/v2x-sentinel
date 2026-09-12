# V2X Sentinel

**Design and Implementation of a Secure V2X Gateway with FOTA Update Capability for Autonomous Vehicle Fleets**

A secure Vehicle-to-Everything (V2X) gateway built on a Raspberry Pi 4 and STM32 microcontrollers, implementing a custom binary protocol (V2X-P) with cryptographic authentication, a fail-safe dual-bank firmware bootloader, and firmware updates that work reliably over UART, CAN bus, and cloud-sourced WiFi.

---

## What This Project Does

- **Custom lightweight binary protocol (V2X-P)** — sync byte, packet ID, length, payload, checksum, with a shared state-machine parser identical across every transport
- **Fail-safe dual-bank bootloader** — firmware updates only take effect after an explicit confirm; a power failure during commit automatically rolls back to the previous, known-good firmware, with no manual recovery needed
- **HMAC-SHA256 firmware authentication** — a from-scratch SHA-256 and HMAC implementation running on the STM32 itself, independently verifying that received firmware was produced by someone holding a shared secret key, not just that it wasn't corrupted
- **Multiple transports, one protocol** — the identical parser accepts commands over direct UART, a physical two-node CAN bus (MCP2515, ISO-TP-style fragmentation), and WiFi via an ESP32 bridge
- **Cloud-to-vehicle firmware delivery** — the Gateway automatically fetches the current firmware from a GitHub-hosted repository over HTTPS, closing the loop from remote storage to a rollback-safe on-vehicle flash write
- **A second, independent ECU node** (STM32 "Black Pill") — proving the protocol generalizes across different hardware, not just one board's implementation
- **Replay attack defense** — sequence-numbered packets, rejecting any previously-captured packet resent later
- **Proactive V2I/V2V accident alerting** — a simulated hazard event triggers a broadcast simultaneously to the Gateway (V2I) and directly to the second node (V2V), the system's first event-driven (not request-response) capability
- **Live hardware status displays** — a 16x2 I2C LCD and an 8x8 MAX7219 dot-matrix module showing real-time bank state and telemetry

## Hardware

| Component | Role |
|---|---|
| Raspberry Pi 4 | Gateway - packet generation, SHA-256/HMAC computation, telemetry |
| STM32F401RE (Nucleo) | Primary ECU node - dual-bank bootloader, FOTA, CAN, WiFi bridge, V2V |
| STM32F401CC ("Black Pill") | Second, independent ECU node |
| ESP32 | WiFi-to-UART bridge (wireless FOTA path) |
| MCP2515 x2 | CAN controller modules (one per CAN-connected node) |
| 16x2 I2C LCD, MAX7219 dot matrix | Live status displays |

## Repository Structure

```
stm32/
  AppA_Merged/Core_Src/   - Primary node firmware: bootloader-compatible app,
                             CAN driver, ISO-TP transport, SHA-256/HMAC,
                             MAX7219 + LCD drivers
  BlackPill/               - Second ECU node firmware
pi/
  gateways/                - Gateway programs (CAN, WiFi, direct-UART to
                             the second node), each with FOTA, HMAC, and
                             replay-attack test options
  cloud/                   - Cloud firmware-fetch script
esp32/
  ESP32_WiFi_UART_Bridge.ino  - Transparent WiFi-to-UART bridge sketch
test_utils/
  test_esp32_bridge.py     - Standalone ESP32 bridge loopback test
  MAX7219_*.ino             - Standalone MAX7219 display bring-up tests
```

## Results

- 100% detection/rejection rate for corrupted, forged (wrong-HMAC), and replayed packets
- 100% automatic recovery across repeated real power-interruption trials, on every verified transport (UART, CAN, WiFi)
- Full real firmware transfers (~11 KB) completed and verified over UART, CAN, and cloud-sourced WiFi
- Verified working across two independent microcontroller boards

## Notes

This repository contains the project's core source files for reference. Board-specific project files (`.ioc`, linker scripts) and full build configuration are not included; each STM32 source file lists its required peripheral configuration (clock source, GPIO pins, peripheral instance) directly in the code.
