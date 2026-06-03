#pragma once
// Copy this file to src/config.h and fill in real values.
// config.h is gitignored — secrets never reach the repo.

// --- Wi-Fi (2.4 GHz only; ESP32 has no 5 GHz radio) ---
#define WIFI_SSID   "YourNetwork"
#define WIFI_PASS   "YourPassword"

// --- Observer location (decimal degrees) ---
#define MY_LAT      48.1351
#define MY_LON      11.5820

// --- Search + behavior tunables ---
#define RADIUS_NM         27      // (legacy) no longer used: the poll radius is now
                                  // derived from the widest range preset (100 km / 54 NM)
#define POLL_INTERVAL_MS  15000   // API poll period (rate limit is 1 req/s)
#define MAX_AIRCRAFT      10      // how many nearest to show on radar / page through
#define IDLE_RETURN_MS    15000   // detail view auto-returns to radar after this idle
#define SWEEP_PERIOD_MS   4000    // radar sweep: ms per full revolution
#define BLE_FRESHNESS_MS  30000   // BLE-fed data is considered live for this long
#define HIDE_GROUND_AIRCRAFT  1   // 1 = hide on-ground aircraft from radar + list

// --- Touch CST816S (I2C) on ESP32-S3-Touch-LCD-1.28 ---
#define TOUCH_SDA  6
#define TOUCH_SCL  7
#define TOUCH_INT  5
#define TOUCH_RST  13
// (GC9A01 LCD pins are configured in platformio.ini via TFT_eSPI build flags.)
