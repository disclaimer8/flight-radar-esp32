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

## Wiring (I2C, PCF8574 backpack)

VCC→3V3, GND→GND, SDA→GPIO21, SCL→GPIO22.

## Troubleshooting (грабли)

- **Backlight on, screen blank** → contrast. Turn the trimmer on the I2C backpack.
- **Garbage / wrong I2C address** → boot serial prints an I2C scan. Set `LCD_ADDR`
  in `config.h` to the found address (`0x27` or `0x3F`).
- **Port not visible** → missing CP210x/CH34x driver, or a charge-only USB cable.
- **`No aircraft`** → normal when the sky is empty; raise `RADIUS_NM` to test.
- **API limit** → don't poll faster than 1 req/s (firmware uses 15 s).
