# Hardware & Bring-up

## Board

**Waveshare ESP32-S3-Touch-LCD-1.28**

| Part | Detail |
|------|--------|
| MCU | ESP32-S3R2 (dual LX7 @ up to 240 MHz), 2 MB PSRAM, 16 MB flash, 512 KB SRAM |
| Display | GC9A01A, 240×240 round IPS, SPI |
| Touch | CST816S capacitive, I2C |
| IMU | QMI8658C (accel + gyro) — **unused** in this firmware |
| Power | USB-C; MX1.25 LiPo header, ETA6096 charger; VBAT sense on GPIO1 |

## Pin map

| Function | GPIO | Where it's set |
|----------|------|----------------|
| LCD MOSI | 11 | `platformio.ini` build flags |
| LCD SCLK | 10 | `platformio.ini` |
| LCD CS | 9 | `platformio.ini` |
| LCD DC | 8 | `platformio.ini` |
| LCD RST | 14 | `platformio.ini` |
| LCD backlight | 2 | `platformio.ini` |
| Touch SDA | 6 | `config.h` |
| Touch SCL | 7 | `config.h` |
| Touch INT | 5 | `config.h` |
| Touch RST | 13 | `config.h` |

No wiring required — the display, touch, and MCU are one integrated board.
Connect it over USB-C and flash.

## Build configuration

The display is driven by [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI),
configured entirely through `build_flags` in `platformio.ini` (no edits inside
the library). Key flags and why they're there:

| Flag | Why |
|------|-----|
| `-DGC9A01_DRIVER` + `TFT_*` pins | Select the round GC9A01 panel and its SPI pins |
| `-DUSE_FSPI_PORT` | **Required on the S3** — see "Boot crash" below |
| `-DARDUINO_USB_MODE=1`, `-DARDUINO_USB_CDC_ON_BOOT=1` | Serial over the S3's native USB |
| `-DBOARD_HAS_PSRAM`, `memory_type = qio_qspi` | Enable the 2 MB PSRAM |
| `-DLOAD_GLCD/FONT2/FONT4` | The fonts the UI actually uses (font 6 is digits-only, so distance uses font 4) |
| `-DSPI_FREQUENCY=40000000` | 40 MHz display SPI |

## Flashing

```bash
pio run -e esp32-s3 -t upload
```

The S3's native USB provides auto-reset, so no BOOT-button hold is needed
(unlike older CH340/CP2102 boards). The port enumerates as
`/dev/cu.usbmodem*` (macOS). To watch serial: `pio device monitor -b 115200`.

## Bring-up gotchas (the ones that actually bit)

### Boot crash: `StoreProhibited` in `TFT_eSPI::init()`
Without an explicit SPI-port flag, the S3 defaults `SPI_PORT` to the `FSPI`
*enum*, which TFT_eSPI's register macros misresolve — `SPI_USER_REG(SPI_PORT)`
points at ~null and the first `writecommand` writes to address `0x10`
(`SET_BUS_WRITE_MODE`), boot-looping. **Fix: `-DUSE_FSPI_PORT`**, which sets the
port to a literal `2` (SPI2), giving a valid peripheral base.
To decode a panic backtrace:
```bash
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/esp32-s3/firmware.elf <addr> <addr> ...
```

### Touch: one tap looked like "nothing happened"
The CST816S **sleeps when idle** (so a blind I2C poll every loop NAKs and stalls
the bus ~50 ms, dropping the frame rate) and emits **many INT events per single
physical touch**, latching the gesture across them (so one tap was read as
several `TG_CLICK`s, toggling the detail view open→shut). The firmware therefore:
1. reads the gesture **only on a falling-edge INT interrupt** (idle = no I2C =
   smooth radar), and
2. **debounces 300 ms** so one physical touch maps to exactly one action.

### Serial over native USB CDC is unreliable for boot logs
After a reset the USB re-enumerates, so a host reader attached to the port often
misses the early boot output. During bring-up, an on-screen debug overlay
(printing INT level / event count / gesture byte) was the dependable diagnostic.

### Sprite memory
The 240×240×16-bit framebuffer is ~115 KB, allocated from internal RAM at boot.
It fits comfortably on the S3R2 alongside Wi-Fi/TLS **and** the NimBLE stack —
all three coexist in internal SRAM without a crash (verified on device), so the
8-bit fallback below isn't needed in practice. If you ever do hit a heap problem
(e.g. `sprite alloc failed` or a crash on the first poll), drop the sprite to
8-bit: `fb.setColorDepth(8)` in `setup()` (~58 KB; TFT_eSPI converts the 16-bit
color constants automatically).

## BLE radio (fallback data path)

The ESP32-S3's radio runs BLE alongside Wi-Fi. The firmware brings up a NimBLE
peripheral (`h2zero/NimBLE-Arduino@^1.4.1`, the lighter 1.x stack) advertising
as `FlightRadar`, so a phone can write aircraft packets when Wi-Fi is
unavailable. The phone side is the Flutter companion app in `companion/`
(Android + iOS); `scripts/ble_send.py` is the laptop smoke-test sender. Wi-Fi
(2.4 GHz) and BLE share the one radio and coexist fine here — the BLE path is
low-duty (one short write at a time), and as noted above the NimBLE stack fits
in SRAM next to the framebuffer and the TLS poll. The v2 wire format caps the
packet at 15 aircraft (492 B) so it lands in a single ATT write at the
negotiated MTU. The protocol and source arbitration are in
[ARCHITECTURE.md](ARCHITECTURE.md).

> NimBLE 1.x uses the single-argument `onWrite(NimBLECharacteristic*)` callback
> signature; the 2.x API changed it. Pin to `^1.4.1` to match the firmware.

### Testing gotcha: the freshness window is short
BLE-fed data is only "live" for `BLE_FRESHNESS_MS` (default 30 s); after that the
radar shows `NO LINK` even though BLE is still connected. When testing manually
with `scripts/ble_send.py`, either send the packet right before you look at the
screen, or temporarily widen `BLE_FRESHNESS_MS` in `config.h` — otherwise the
window can expire before you've finished reading the device's serial log.

## Constraint: North-up only

The board's IMU has no magnetometer, so the radar cannot rotate to the device's
physical heading. The top of the screen is always **geographic North**, and each
aircraft's compass label is the bearing from your configured coordinates. A
heading-up mode would require an external compass module (out of scope).
