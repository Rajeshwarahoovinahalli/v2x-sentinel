# Wiring Reference

Complete pin mapping across every board and module in the system.

## STM32 Nucleo (Primary Node) - Peripheral Summary

| Peripheral | Pins | Connects To |
|---|---|---|
| SPI1 | PA4 (CS), PA5 (SCK), PA6 (MISO), PA7 (MOSI) | MCP2515 (CAN controller) |
| USART1 | PA9 (TX), PA10 (RX) | Black Pill USART2 (direct V2V link) |
| USART2 | PA2 (TX), PA3 (RX) | ST-Link Virtual COM Port (debug output) |
| USART6 | PA11 (TX), PA12 (RX) | ESP32 (WiFi bridge) |
| SPI2 | PB12 (CS), PB13 (SCK), PB14 (MISO), PB15 (MOSI) | MAX7219 dot-matrix display |
| I2C1 | PB6 (SCL), PB7 (SDA) | 16x2 LCD (I2C backpack) |
| GPIO | PB0 | Onboard LED (bank/heartbeat indicator) |
| GPIO | PC13 | User Button B1 (short press = accident alert, long press = manual bank switch) |

## Black Pill (Second Node) - Peripheral Summary

| Peripheral | Pins | Connects To |
|---|---|---|
| USART1 | PA9 (TX), PA10 (RX) | Raspberry Pi, via USB-to-TTL adapter |
| USART2 | PA2 (TX), PA3 (RX) | STM32 Nucleo USART1 (direct V2V link) |
| GPIO | PC13 | Onboard LED (status / hazard warning indicator) |

**Note:** on the Black Pill, USART1 and USART2 each connect to a different device - do not
cross-wire them with the Nucleo's pins of the same name/number.

## MCP2515 CAN Modules (one per CAN-connected node)

### Nucleo-side module -> Nucleo SPI1
| MCP2515 | Nucleo |
|---|---|
| VCC | 5V |
| GND | GND |
| CS | PA4 |
| SCK | PA5 |
| MISO | PA6 |
| MOSI | PA7 |

### Pi-side module -> Raspberry Pi SPI0 (standard Linux CAN overlay pins)
| MCP2515 | Raspberry Pi |
|---|---|
| VCC | 5V |
| GND | GND |
| CS | GPIO8 (CE0) |
| SCK | GPIO11 |
| MISO | GPIO9 |
| MOSI | GPIO10 |
| INT | GPIO25 |

### Between the two MCP2515 modules (the CAN bus itself)
- CAN-H <-> CAN-H
- CAN-L <-> CAN-L
- A direct GND wire between the Nucleo board and the Raspberry Pi board (separate from each
  board's own module ground) - required for correct differential signal reference
- Both modules' onboard 120 ohm termination jumpers enabled

## ESP32 (WiFi Bridge)

| ESP32 | Nucleo |
|---|---|
| GPIO17 (TX) | PA12 (USART6 RX) |
| GPIO16 (RX) | PA11 (USART6 TX) |
| GND | GND |

Powered independently via its own USB cable - not powered from the Nucleo.

## Raspberry Pi <-> Black Pill

A standard USB-to-TTL (CP2102 or similar) adapter, plugged into a Raspberry Pi USB port:

| Adapter | Black Pill |
|---|---|
| TX | PA10 (RX) |
| RX | PA9 (TX) |
| GND | GND |

## ST-Link V2 <-> Black Pill (SWD Programming)

Only 4 of the ST-Link's 20 pins are used. Pin numbering varies by dongle - verify against your
specific unit's printed pinout before wiring.

| ST-Link | Black Pill |
|---|---|
| SWCLK | SWCLK |
| GND | GND |
| 3V3 (VTref) | 3V3 |
| SWDIO | SWDIO |

The Nucleo has its ST-Link built in and needs no external programmer.

## MAX7219 Dot-Matrix Display

Many common clone modules (including "MH"-branded boards) have **two** header rows, labelled
`IN` and `OUT`. Always wire to the **`IN`** side - the `OUT` side is for daisy-chaining a second
module and will not work as a control input.

| MAX7219 (IN side) | Nucleo |
|---|---|
| VCC | 5V |
| GND | GND |
| DIN | PB15 (SPI2 MOSI) |
| CS | PB12 |
| CLK | PB13 (SPI2 SCK) |

## 16x2 I2C LCD

| LCD Backpack | Nucleo |
|---|---|
| VCC | 5V |
| GND | GND |
| SCL | PB6 |
| SDA | PB7 |

Default I2C address assumed is `0x27`; if the display shows nothing or garbage, the backpack may
use `0x3F` instead (the other common default).
