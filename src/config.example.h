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

// --- LCD 1602 (parallel HD44780, 4-bit mode) ---
#define LCD_RS  19
#define LCD_EN  23
#define LCD_D4  18
#define LCD_D5  25
#define LCD_D6  26
#define LCD_D7  27
