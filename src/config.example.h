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
#define RADIUS_NM         30      // search radius, nautical miles (<=250)
#define POLL_INTERVAL_MS  15000   // API poll period (rate limit is 1 req/s)
#define CYCLE_INTERVAL_MS 5000    // per-aircraft screen time
#define MAX_AIRCRAFT      5       // how many nearest to rotate through

// --- LCD (PCF8574 I2C backpack) ---
#define LCD_ADDR    0x27          // try 0x3F if 0x27 shows nothing
#define LCD_SDA     21
#define LCD_SCL     22
