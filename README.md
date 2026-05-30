# Flight Ticker

ESP32 + 1602 LCD that shows the nearest aircraft from airplanes.live.

## Setup

1. Install PlatformIO Core: `brew install platformio`
2. `cp src/config.example.h src/config.h` and fill in Wi-Fi (2.4 GHz only),
   your lat/lon, and `RADIUS_NM`.
3. Run the host tests: `pio test -e native -f test_core`
4. Build: `pio run -e esp32dev`
5. Find the port: `ls /dev/cu.*` (CH340 → `cu.usbserial-*`, CP2102 → `cu.SLAB_USBtoUART`).
   No port? Install the CP210x or CH34x driver and reconnect USB.
6. Flash: `pio run -e esp32dev -t upload`. If upload stalls, hold **BOOT** as it
   starts "Connecting...".
7. Monitor: `pio device monitor -b 115200`.

## Wiring (parallel HD44780, 4-bit)

Bare 1602, 16-pin header → ESP32. A 10k pot on VO sets contrast.

| LCD pin | → | ESP32 |
|---------|---|-------|
| 1 VSS   | → | GND |
| 2 VDD   | → | 5V (VIN) |
| 3 VO    | → | 10k pot wiper (pot ends to 5V and GND) |
| 4 RS    | → | GPIO19 |
| 5 RW    | → | GND |
| 6 E     | → | GPIO23 |
| 11 D4   | → | GPIO18 |
| 12 D5   | → | GPIO25 |
| 13 D6   | → | GPIO26 |
| 14 D7   | → | GPIO27 |
| 15 A (BLA) | → | 5V via ~220Ω |
| 16 K (BLK) | → | GND |

Pins 7–10 (D0–D3) are left unconnected in 4-bit mode. Pin assignments live in
`config.h` (`LCD_RS/EN/D4..D7`).

## Troubleshooting (грабли)

- **Backlight on, screen blank or full white blocks** → contrast. Turn the VO pot.
  No pot? Tie VO to GND through ~1k (or directly) for high contrast.
- **Garbage characters** → check D4–D7 / RS / E wiring order and a solid common GND.
  If still garbled, try powering VDD from 3V3 instead of 5V (matches the ESP32's
  3.3V logic levels).
- **Port not visible** → missing CP210x/CH34x driver, or a charge-only USB cable.
- **`No aircraft`** → normal when the sky is empty; raise `RADIUS_NM` to test.
- **API limit** → don't poll faster than 1 req/s (firmware uses 15 s).
