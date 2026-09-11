# Family Calendar LVGL — Coding Agent Handoff

**Prepared:** 2026-09-11  
**Current recommended baseline:** **v1.6.1**  
**Next target:** **v1.7.0**  
**User project path:** `/Users/justin/SynologyDrive/Development/Arduino/ESP32 P4/family_calendar_lvgl`

---

## 1. Purpose of this handoff

The user wants development moved to a coding agent.  This document is the source-of-truth context for continuing the **Family Calendar LVGL** project without repeating earlier hardware, networking, packaging, or UI mistakes.

The immediate requested feature set for **v1.7.0** is:

1. Add a **Weather** tab, visually inspired by `8bitmcu/ESPHome_WeatherDisplay`, but implemented natively in this Arduino/LVGL project.
2. Use the **Meteorologisk institutt / MET Norway Locationforecast 2.0** API (`api.met.no`) directly.
3. Add a small, Outlook-style daily weather summary in the **Calendar** day headers.
4. Re-enable **automatic selected-week ±1 week refresh** after calendar week navigation, but do so using the network-safe architecture that stabilized the device.

The user specifically asked for weather similar in appearance and functionality to:

- https://github.com/8bitmcu/ESPHome_WeatherDisplay

**Important licensing note:** that repository currently reports no repository-level license.  Treat it as **visual/UX inspiration only**.  Do not copy its source files, configuration, images, fonts, or other assets into this project unless licensing is explicitly verified.  Re-create the layout and behavior independently.

---

## 2. Baseline / do not start from the aborted 1.7.0 work folder

The latest known complete source package is:

- `family_calendar_lvgl_1.6.1_update.zip`
- SHA-256: `67537be7259363ead29c754cb7a9eeacc7a848460081027645f75c249e967c97`

The temporary environment folder named:

- `/mnt/data/family_calendar_lvgl_1.7.0_work`

is **not a usable implementation**.  It was created during an interrupted weather attempt and only contains partial scaffolding (`README.md` / `partitions.csv`).  Do not use it as the development baseline.

The coding agent should work from the user's actual current project folder after v1.6.1 has been applied, or from the complete v1.6.1 update ZIP if a local copy is needed.

---

## 3. Non-negotiable update/package rules

All future releases are **in-place update ZIPs** extracted directly over the existing project directory.  The ZIP must contain project files at the **ZIP root**, not inside a version-named enclosing folder.

Never include or overwrite these persistent local files:

- `include/app_secrets.h`
- `include/app_local.h`

The project intentionally ships only examples:

- `include/app_secrets.example.h`
- `include/app_local.example.h`

The user's Wi-Fi credentials, Home Assistant token/base URL, calendar mappings, and local preferences must survive every update.

For code/firmware changes, the user strongly prefers **complete updated files / a complete update ZIP**, not isolated snippets.

Typical user workflow:

```bash
pio run -t clean
pio run -t upload
pio device monitor -b 115200
```

Do not claim the firmware compiled unless PlatformIO was actually run successfully.  Previous assistant-side environments did not have a usable PlatformIO toolchain.

---

## 4. Hardware — exact device and known-good display path

### Device

Exact rear label confirmed by the user:

```text
SKU: 10153001 (2624)
P4-10.1 inch Capacitive touch
Model: JC8012P4A1C_I_W_Y
Resolution: 800x1280
```

Hardware:

- ESP32-P4 application processor
- ESP32-C6 companion processor for Wi-Fi/Bluetooth via ESP-Hosted
- JD9365 MIPI-DSI LCD
- Native portrait: 800x1280
- Application orientation: landscape 1280x800
- GSL3680 capacitive touch
- 32 MB PSRAM
- 16 MB flash

Known pins:

- Backlight: GPIO 23
- LCD reset: GPIO 27
- Touch SDA: GPIO 7
- Touch SCL: GPIO 8
- Touch reset: GPIO 22
- Touch interrupt: GPIO 21
- C6 hosted reset: GPIO 54
- C6 hosted CMD: GPIO 19
- C6 hosted CLK: GPIO 18
- C6 hosted D0..D3: GPIO 14, 15, 16, 17

### Display driver — do not disturb this without direct evidence

Known-good driver source:

- https://github.com/chipguy/JC8012P4A1C_display
- pinned SHA: `71cc9af19ef63c1761d1139069e3a1ece6682dfa`

The project patches that driver at build time for the exact 2624 / `JC8012P4A1-V2` JD9365 init/timing profile.

Critical known-good panel parameters:

- pclk: 70 MHz
- lane rate: 1500 Mbps
- horizontal: 20 / 20 / 40
- vertical: 4 / 10 / 20
- RGB565 / 16-bit
- native panel: 800x1280

Landscape rendering is **CPU pixel-perfect rotation**, not PPA rotation.  PPA rotation was tried and was visibly blurry.

`src/board_lvgl.cpp` currently uses:

- one full-screen LVGL render buffer
- two native panel framebuffers
- exact RGB565 CPU rotation
- framebuffer switching
- `esp_cache_msync(..., ESP_CACHE_MSYNC_FLAG_DIR_C2M)` before display swap

Do not replace the display pipeline while working on weather/network/UI features.

---

## 5. Toolchain / PlatformIO baseline

Current baseline is pinned to:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip
board = esp32-p4
framework = arduino
monitor_speed = 115200
upload_speed = 230400
```

Framework stack:

- Arduino-ESP32 3.3.8
- ESP-IDF 5.5.4
- LVGL 9.3.0
- ArduinoJson 7.4.3

Current libraries:

```ini
lib_deps =
    lvgl/lvgl@9.3.0
    bblanchon/ArduinoJson@7.4.3
```

JPEGDEC was removed when live camera support was rolled back.  Do **not** re-add JPEG/image decoding for weather unless there is a compelling reason.  Prefer low-cost LVGL-drawn weather icons.

The custom 16 MB partition layout keeps dual ~6 MB OTA application slots.

---

## 6. ESP32-C6 / ESP-Hosted status

The P4 has no onboard Wi-Fi radio.  Networking goes through the C6 companion.

Known-good ESP-Hosted versions:

```text
Host firmware version: 2.12.3
Slave firmware version: 2.12.3
Versions Match!
```

The build embeds/stages the official matching C6 image and can update an older C6 automatically.

**Do not propose C6 reflashing or firmware changes unless new logs specifically show a version mismatch or C6 firmware problem.**

---

## 7. Critical stability history — read before adding any networking

This is the most important context in the entire project.

### What failed

The project became unstable when too much HTTPS/network work was added through ESP-Hosted.  The failures included:

- `sdio_rx_get_buffer` assertions inside ESP-Hosted/SDIO
- task watchdog resets where `IDLE0` was starved
- camera worker stack overflow
- Home Assistant worker stalls during HTTPS requests

Live camera support was removed entirely because it was too costly and unstable.

### What stabilized the device

The key changes that produced the current stable architecture were:

1. **No live camera feed** and no image/JPEG decoding.
2. Home Assistant networking uses a **single background worker**.
3. HA worker priority is **0 (idle priority)**.
4. HA task stack is **16 KB**.
5. Short network timeouts:
   - connect: 2 s
   - request/read: 2.5 s
6. Calendar fetches use one consolidated Home Assistant `calendar.get_events` request for all configured calendars instead of multiple sequential requests.
7. Hidden dashboards do not generate recurring traffic.
8. Automatic refreshes are tied to the **currently visible tab** only.
9. Screen-off pauses active-tab automatic requests.
10. Boot calendar sync waits until Wi-Fi has been continuously connected for 10 seconds.
11. Wi-Fi power saving is disabled; automatic reconnect is enabled.

### Current active-tab policy

From `app_config.h`:

```cpp
#define HA_ACTIVE_TAB_SETTLE_MS 1500UL
#define HA_ACTIVE_TAB_CALENDAR_REFRESH_MS 300000UL  // 5 min
#define HA_ACTIVE_TAB_CHORES_REFRESH_MS    60000UL  // 1 min
#define HA_ACTIVE_TAB_ALARM_REFRESH_MS     15000UL  // 15 sec
```

**Do not add a second network worker that can make concurrent HTTPS requests.**  Weather must be serialized with Home Assistant network traffic.

Preferred low-risk implementation:

- Keep one effective outbound network execution path.
- Add weather request flags/data handling to the existing worker model, or create a shared central network job worker and migrate carefully.
- Do not let `weather_service` independently open TLS while `ha_worker` may also be using ESP-Hosted.

The smallest-risk v1.7.0 design is to let `weather_service` own parsing/model logic but have the existing background worker execute the blocking weather fetch when a weather request flag is queued.

---

## 8. Current application architecture

### Startup

`src/main.cpp` performs roughly:

1. Serial
2. display/LVGL
3. backlight
4. time service
5. calendar PSRAM cache
6. demo calendar model
7. chores
8. Alarmo
9. UI
10. ESP-Hosted network
11. legacy camera/wake service shell (live feed disabled)
12. Home Assistant worker

Main loop:

- `network_service_loop()`
- `home_assistant_loop(calendar_ui_week_offset())`
- publish HA changes to UI
- legacy camera/wake service loop
- UI loop
- LVGL/board loop
- 5 ms delay

### Important source files

- `src/board_lvgl.cpp` — display, rotation, touch, backlight
- `src/calendar_ui.cpp` — all dashboards and major LVGL UI
- `src/calendar_model.cpp` — calendar event model and 16-week PSRAM LRU cache
- `src/home_assistant.cpp` — HA worker, calendar, chores, Alarmo network access
- `src/network_service.cpp` — ESP-Hosted Wi-Fi state, RSSI, reconnect
- `src/time_service.cpp` — local Central time, SNTP-aware time helpers
- `src/chore_service.cpp` — cached todo items / local state
- `src/alarm_service.cpp` — cached Alarmo state
- `src/camera_service.cpp` — live camera path removed; only legacy wake-sensor scaffolding remains

### Current Home Assistant change flags

`include/home_assistant.h`:

```cpp
HA_CHANGE_CALENDAR
HA_CHANGE_CHORES
HA_CHANGE_TODO_LISTS
HA_CHANGE_ALARM
HA_CHANGE_ALARM_PANELS
```

Weather should have its own service/change flags rather than overloading HA flags unless weather is intentionally integrated into the same worker queue.

---

## 9. Current UI state (v1.6.1)

Dashboard enum currently:

```cpp
enum class Dashboard : uint8_t {
    Calendar = 0,
    Chores,
    Meals,
    Alarm,
    Settings,
};
```

Current footer has five tabs with ~236 px button width.

Recommended v1.7.0 order:

```text
Calendar | Weather | Chores | Meals | Alarm | Settings
```

For six tabs, the project previously used a compact geometry successfully around:

```cpp
BUTTON_W = 202
GAP = 8
TOTAL_W = 202 * 6 + 8 * 5 = 1252
```

Use that proven layout rather than squeezing six 236 px buttons.

### Header

v1.6.0/1.6.1 added:

- Wi-Fi status chip
- ON/OFF state
- four RSSI strength bars
- drawn sun/moon light/dark mode toggle

Those features should remain.

### Calendar swipe

v1.6.1 intentionally does **not** use LVGL gesture velocity detection.

Instead `service_calendar_swipe()` reads mapped touch state from `board_get_touch_state()` and recognizes a swipe on finger release:

- min horizontal travel: 70 px
- max vertical drift: 120 px
- horizontal distance must exceed vertical distance

Swipe left = next week.  Swipe right = previous week.

Preserve this distance-based implementation.

---

## 10. Current calendar data path

`calendar_model` uses a **16-week PSRAM LRU cache**.

`sync_calendar_window(center_offset)` already does exactly what is needed for ±1 week loading:

- selected week
- previous week
- next week

It builds one three-week time range and uses a **single** HA service call:

```text
POST /api/services/calendar/get_events?return_response
```

Target is an array of all configured calendar entities.

This consolidated request is significantly safer than the old sequential `/api/calendars/<entity>` model.

At present, week navigation itself is RAM/cache-only.  The user now explicitly wants automatic ±1 week refresh re-enabled when changing weeks.

---

## 11. Requested v1.7.0 feature A — Weather tab using MET Norway

### API

Use:

```text
https://api.met.no/weatherapi/locationforecast/2.0/compact
```

Query parameters:

```text
lat=<latitude>
lon=<longitude>
altitude=<optional integer meters>
```

Use **no more than 4 decimals** for latitude/longitude.

Official docs:

- https://api.met.no/weatherapi/locationforecast/2.0/documentation
- https://docs.api.met.no/doc/locationforecast/HowTO.html
- https://docs.api.met.no/doc/TermsOfService.html
- https://docs.api.met.no/doc/License.html

### Required User-Agent

MET Norway requires an identifying, non-generic User-Agent containing meaningful application/contact information.  Missing/generic identification can return HTTP 403.

Do **not** invent a fake email or contact string.

Add persistent local configuration to `app_local.example.h`, but do not include the user's real `app_local.h` in updates:

```cpp
// Weather location. Coordinates are sent to api.met.no rounded to max 4 decimals.
#define WEATHER_LATITUDE  0.0
#define WEATHER_LONGITUDE 0.0
// #define WEATHER_ALTITUDE_M 230

// Must be a legitimate identifying User-Agent/contact per MET Norway policy.
// Example only; user must customize.
#define METNO_USER_AGENT "FamilyCalendar/1.7.0 https://example.com/contact"

#ifndef WEATHER_USE_IMPERIAL
#define WEATHER_USE_IMPERIAL 1
#endif
```

Better behavior: if coordinates or User-Agent are not meaningfully configured, show **Weather not configured** and do not send a request.  Do not silently fetch `(0,0)`.

### Caching requirements

MET Norway explicitly requires clients to honor response cache metadata.

Capture and persist in the weather service state:

- `Expires`
- `Last-Modified`

Rules:

1. Do not repeat a request before `Expires`.
2. Once expired, send `If-Modified-Since: <previous Last-Modified>`.
3. Handle `304 Not Modified` without re-parsing/replacing the forecast.
4. Handle `429` with long backoff.
5. Log `203` as a deprecation warning.
6. Handle `403` as a configuration/User-Agent/coordinates problem.
7. Support redirects and gzip/deflate as required by MET Norway.

If response headers are missing/unparseable, use a conservative fallback refresh interval (e.g. 30–60 minutes), not a rapid timer.

### Attribution

The Weather screen should visibly include small text such as:

```text
Data from MET Norway
```

MET Norway requests attribution.  Do not present the app as an official MET/Yr product and do not use the Yr logo/name as branding.

### Network serialization

Do not launch weather HTTPS concurrently with Home Assistant HTTPS.

Recommended design:

- `weather_service` owns model/cache/UI-facing data.
- `weather_service_request_refresh()` sets a pending request flag.
- Existing single network worker processes either HA work or weather work, never both concurrently.
- Weather fetch runs at the same safe priority and timeout philosophy as HA.
- Add a small inter-request gap after any HA or MET request.

If a shared worker extraction is too invasive for v1.7.0, minimally call a blocking `weather_service_worker_fetch()` from the existing worker loop when HA queues are idle.

### Avoid large raw strings

Do not download the whole compact response into a huge `String` unless testing proves it harmless.

Prefer:

- `HTTPClient` stream
- ArduinoJson filtered deserialization
- retain only fields the UI needs
- keep final parsed weather model in PSRAM or compact fixed structs

The compact forecast can contain many hourly points.  Avoid duplicating it several times in heap memory.

---

## 12. Suggested weather model

Create:

- `include/weather_service.h`
- `src/weather_service.cpp`

Optional separate `weather_model.h/.cpp` only if it improves clarity.

Suggested structures:

```cpp
struct WeatherCurrent {
    bool valid;
    time_t epoch;
    float temperature_c;
    int relative_humidity_pct;
    float pressure_hpa;
    float wind_mps;
    float wind_direction_deg;
    char symbol_code[40];
};

struct WeatherHour {
    bool valid;
    time_t epoch;
    float temperature_c;
    float precipitation_mm;
    int precipitation_probability_pct;
    float wind_mps;
    char symbol_code[40];
};

struct WeatherDay {
    bool valid;
    time_t local_day;
    float high_c;
    float low_c;
    float precipitation_mm;
    int precipitation_probability_max_pct;
    float wind_max_mps;
    char symbol_code[40];
};
```

Keep only enough hourly records for the weather UI (e.g. next 18–24 hours) and ~9 daily summaries.

### Daily aggregation

MET Locationforecast is time-series based rather than a ready-made daily forecast.  Build daily summaries locally:

- convert each forecast timestamp to local time using the existing Central timezone setup
- group by local calendar date
- high = max hourly/instant temperature
- low = min hourly/instant temperature
- precip = sum available `next_1_hours.details.precipitation_amount` for that local date where present
- precip probability = max probability value where present
- wind = max wind speed
- high-level daily condition = choose a representative daytime symbol (prefer roughly 12:00–15:00 local) or a severity-based aggregation

The Locationforecast API provides forecasts roughly up to 9 days.  Therefore calendar weather indicators should only appear for days actually covered by the forecast.  Past days and dates beyond the forecast horizon should simply omit the weather indicator.

---

## 13. MET symbol handling / icons

Do not use emoji or arbitrary Unicode weather glyphs.  This project has already produced LVGL missing-glyph warnings for punctuation/symbols outside enabled fonts.

Use one of these approaches:

1. Preferred: draw a small family of weather icons with LVGL primitives.
2. Acceptable: compile a small, explicitly licensed static icon set into RGB565 assets.
3. MET Norway weather icons are MIT-licensed, but if imported, preserve required copyright/license notices and keep memory impact small.

For the simplest robust implementation, map symbol codes into broad categories:

- clear / fair
- partly cloudy
- cloudy
- fog
- rain / showers
- heavy rain
- sleet
- snow
- thunder

Normalize suffixes such as:

- `_day`
- `_night`
- `_polartwilight`

Map broad families to a reusable `WeatherIconKind` enum and draw the same icon at two sizes:

- calendar: ~22–28 px
- Weather tab: ~56–80 px

---

## 14. Weather tab layout

The user likes the look/functionality of `ESPHome_WeatherDisplay`.  Re-create that concept in the existing Family Calendar visual language rather than making a separate black/glass-style theme.

Suggested 1280x800 layout under the existing 84 px header and 64 px footer:

### Top current-weather card (~260–300 px high)

Left/center:

- large weather icon
- current condition text
- current temperature, large

Metrics in 3–4 columns:

- high / low
- humidity
- precipitation / probability
- wind speed/direction
- pressure
- updated time

Do not show sunrise/sunset from MET Locationforecast unless implemented via a clearly separate local astronomical calculation.  Locationforecast itself does not supply sunrise/sunset.

### Lower daily forecast row

Use 6 compact cards, similar in spirit to the reference project:

- weekday/date
- weather icon
- short condition
- high / low
- precipitation
- max wind

Optional later enhancement: hourly view.  Do not overload v1.7.0 unless time remains after stability is proven.

### Refresh behavior

Weather data should refresh when:

- no forecast is available after Wi-Fi becomes stable
- forecast is expired according to MET `Expires`
- user enters Weather tab and data is expired
- Calendar is visible and weather is expired (because calendar needs indicators)

Do **not** poll frequently.  If the cached MET response has not expired, entering the Weather tab should use cached data immediately and send no request.

Provide a manual Weather Refresh control, but if data is still before `Expires`, show that cached data is still fresh rather than violating MET cache requirements.

---

## 15. Requested v1.7.0 feature B — Outlook-style calendar weather indicators

The Calendar currently has seven day columns.  Event cards begin around y=66 inside each day column.

Add a compact weather strip in each day header for dates with forecast data.

Recommended presentation:

```text
SUN
Sep 13
[small icon] 72 / 54°
```

or a single compact row next to the date if space is tight.

Requirements:

- icon drawn with LVGL primitives or a licensed static asset
- high/low in configured display units
- no extra network request per day
- all seven headers use the single cached weather model
- no indicator for dates outside available forecast horizon
- no placeholders that consume space when forecast is unavailable

Do not let the indicator materially reduce the existing event capacity.  If needed, increase header area only slightly and validate six visible event cards still fit without clipping.

Calendar redraw when weather changes:

- add a weather-change flag
- if Calendar is visible, call `render_week()` when weather data publishes
- if Weather tab is visible, rebuild/update weather controls

---

## 16. Requested v1.7.0 feature C — re-enable automatic ±1 week refresh after navigation

The existing backend already fetches selected week ±1 using one safe `calendar.get_events` request.  Re-enable only the **scheduling trigger**, not the old multi-request implementation.

### Desired user behavior

When the user changes weeks by:

- Previous button
- Next button
- swipe left/right
- Today button

then:

1. immediately activate/render a cached week if available
2. do not block the UI
3. wait for navigation to settle
4. automatically refresh selected week + previous + next week in the background
5. do not issue multiple requests when the user rapidly swipes/taps through several weeks

### Safe implementation

Add a common helper, e.g.:

```cpp
void set_week_offset(int new_offset, WeekChangeReason reason);
```

Use it from buttons, Today, and swipe code so all navigation paths behave identically.

Track:

```cpp
uint32_t g_week_navigation_changed_ms;
int g_pending_nav_refresh_offset;
bool g_nav_refresh_pending;
```

Recommended debounce:

```cpp
#define HA_CALENDAR_NAV_DEBOUNCE_MS 1500UL
```

After the debounce expires, queue **one** centered calendar refresh only if:

- Calendar dashboard is still visible
- display is awake
- Wi-Fi has passed the existing stability window
- HA/network worker is idle
- there is no current calendar request
- selected offset is still the same offset that settled

### Reduce redundant fetches

Before queuing, check the three cache weeks around the selected week.

If all three are fresh (for example, refreshed within the active-tab 5-minute interval), skip the navigation refresh.

This prevents bouncing among recently loaded weeks from repeatedly hitting Home Assistant.

Suggested helper:

```cpp
bool home_assistant_calendar_window_needs_refresh(int center_offset,
                                                   uint32_t max_age_ms);
```

Use existing `calendar_cache_has()` / `calendar_cache_is_fresh()`.

### Very important

Do not resurrect the old design that performed multiple calendar HTTPS calls.  Only call the current consolidated `sync_calendar_window(center_offset)` path.

Do not allow Weather and HA refreshes to overlap.

---

## 17. Active-tab refresh policy after adding Weather

Extend current policy rather than creating hidden global polling.

Recommended:

- Calendar visible:
  - calendar auto refresh every 5 min
  - weather refresh only when MET cache is expired
- Weather visible:
  - weather refresh only when expired
- Meals visible:
  - calendar auto refresh every 5 min
- Chores visible:
  - chores every 1 min
- Alarm visible:
  - Alarmo every 15 sec
- Settings visible:
  - local status only, no recurring network
- display asleep:
  - no active-tab recurring network

Navigation-triggered calendar prefetch is the only new exception, and should only occur after the Calendar page settles.

---

## 18. Suggested config additions

In `app_config.h`, defaults only; local overrides live in `app_local.h`.

Suggested additions:

```cpp
#ifndef WEATHER_USE_IMPERIAL
#define WEATHER_USE_IMPERIAL 1
#endif

#ifndef METNO_HTTP_CONNECT_TIMEOUT_MS
#define METNO_HTTP_CONNECT_TIMEOUT_MS 2000
#endif
#ifndef METNO_HTTP_TIMEOUT_MS
#define METNO_HTTP_TIMEOUT_MS 2500
#endif

#ifndef WEATHER_FALLBACK_REFRESH_MS
#define WEATHER_FALLBACK_REFRESH_MS 3600000UL
#endif

#ifndef HA_CALENDAR_NAV_DEBOUNCE_MS
#define HA_CALENDAR_NAV_DEBOUNCE_MS 1500UL
#endif
#ifndef HA_CALENDAR_NAV_REFRESH_MAX_AGE_MS
#define HA_CALENDAR_NAV_REFRESH_MAX_AGE_MS 300000UL
#endif
```

Do not hard-code the user's coordinates/contact in versioned files.

---

## 19. Units

The user is in the United States, so the expected display default is:

- Fahrenheit
- mph
- inches only if precipitation is displayed as accumulation; otherwise mm is acceptable but Fahrenheit/mph is preferable for consistency

However, keep a compile-time/local override so metric is easy later.

Conversion helpers:

```cpp
F = C * 9.0f / 5.0f + 32.0f
mph = mps * 2.236936f
inches = mm / 25.4f
```

Do not convert source/model values in-place; keep canonical SI internally and format units in the UI.

---

## 20. Weather time parsing

`time_service.cpp` already owns timezone and local-date arithmetic, but UTC ISO parsing currently exists privately inside `home_assistant.cpp`.

For weather, do not duplicate a third timestamp parser if avoidable.

Good cleanup:

- move a generic UTC/ISO8601 parser into `time_service.h/.cpp`
- let HA and Weather use the same parser

Do this only if it can be tested without destabilizing calendar behavior.  A small duplicate parser inside weather service is acceptable for v1.7.0 if shared refactoring creates risk.

---

## 21. Memory / stack rules

Past camera work proved that large local objects can overflow FreeRTOS stacks.

Rules:

- no large decoder/parser structs on worker stack
- no multi-megabyte frame buffers
- prefer PSRAM for larger forecast arrays
- fixed-size structs are preferred over many `String`s
- do not keep both raw MET JSON and a parsed full model simultaneously
- log PSRAM allocation failure clearly and fail weather gracefully

Keep network worker stack at least 16 KB unless measured evidence supports another value.

---

## 22. UI style rules learned the hard way

The current UI had earlier clipping/spacing problems.  Preserve these patterns:

- generic panels/buttons have explicit zero padding unless intentionally overridden
- avoid depending on LVGL default theme padding
- use `LV_SIZE_CONTENT` / explicit label heights carefully
- avoid unsupported Unicode symbols; several curly quotes/em-dashes produced missing glyph logs
- use ASCII punctuation or known enabled glyphs
- sun/moon and new weather graphics should use LVGL primitives or explicit image assets

Existing geometry:

```cpp
SCREEN_W = 1280
SCREEN_H = 800
HEADER_H = 84
FOOTER_H = 64
PAGE_Y = HEADER_H + 8
PAGE_H = FOOTER_Y - PAGE_Y - 8
```

Calendar:

```cpp
CAL_TOOLBAR_H = 62
CAL_MAIN_Y = HEADER_H + CAL_TOOLBAR_H + 10
CAL_SIDE_W = 210
CAL_X = CAL_SIDE_W + 12
CAL_W = SCREEN_W - CAL_X - 8
DAY_GAP = 4
DAY_W = (CAL_W - DAY_GAP * 6) / 7
EVENT_CARD_H = 76
EVENT_CARD_STEP = 80
MAX_VISIBLE_EVENTS_PER_DAY = 6
```

---

## 23. Files likely to add/modify for v1.7.0

### Add

- `include/weather_service.h`
- `src/weather_service.cpp`

Optional:

- `include/weather_icons.h`
- `src/weather_icons.cpp`

### Modify

- `include/app_config.h`
  - version `1.7.0`
  - weather defaults
  - navigation refresh debounce/freshness settings
- `include/app_local.example.h`
  - weather location / User-Agent examples
- `src/main.cpp`
  - initialize/weather service and publish weather changes
- `src/calendar_ui.cpp`
  - Weather dashboard enum/tab/footer
  - Weather screen UI
  - Calendar weather header indicators
  - common week-navigation helper
  - queue debounced nav refresh
  - active-tab weather scheduling
- `include/home_assistant.h`
  - expose a centered calendar refresh request and/or freshness helper
  - possibly expose worker/network-idle coordination
- `src/home_assistant.cpp`
  - safe navigation refresh scheduling interface
  - process weather fetch in shared worker if using the recommended single-worker approach
- `src/time_service.*`
  - only if extracting shared ISO parser
- `README.md`
  - v1.7.0 notes and weather setup

Potentially remove stale camera naming only if convenient; do not make a large unrelated cleanup release.

---

## 24. Logging requirements

Add concise, highly diagnostic logs.  Examples:

```text
[Weather] Request queued: no cached forecast
[Weather] GET locationforecast compact lat=44.xxxx lon=-88.xxxx
[Weather] HTTP 200 in 842ms expires=... last-modified=...
[Weather] Parsed current + 9 daily + 18 hourly forecast points
[Weather] HTTP 304; cached forecast retained
[Weather] Request skipped; cache valid for another 23m
[Calendar] Week changed to offset 3; nav refresh debounce started
[Calendar] Nav settled at offset 3; refreshing selected week +/-1
[Calendar] Nav refresh skipped; three-week window is fresh
```

Never log:

- Home Assistant bearer token
- Wi-Fi password
- sensitive secret file contents

Past crash dumps accidentally exposed an HA bearer token from stack memory.  Treat secrets carefully.

---

## 25. Acceptance tests for v1.7.0

### Build/package

- clean PlatformIO build succeeds
- update ZIP root contains project files directly
- ZIP excludes `include/app_local.h`
- ZIP excludes `include/app_secrets.h`
- example local/secrets files remain

### Boot/stability

- display starts normally
- ESP-Hosted host/slave remains 2.12.3 matched
- HA boot calendar sync still succeeds
- no task watchdog reset after 10+ minutes idle
- no SDIO assertion
- no stack protection fault

### Weather

- with valid weather config, MET request succeeds
- User-Agent is present and valid
- coordinates have <=4 decimal places
- `Expires` / `Last-Modified` captured
- no refresh before Expires
- 304 handled correctly
- weather renders after boot without blocking UI
- Weather tab shows current conditions + daily cards
- `Data from MET Norway` attribution visible
- forecast data remains cached while switching tabs
- no request on every tab entry if cache is still valid

### Calendar weather indicators

- current/near-future dates show icon + high/low
- dates outside MET forecast range do not show fake data
- event cards remain unclipped
- theme switch redraws weather icons correctly
- weather refresh updates visible Calendar without requiring reboot

### Calendar navigation + prefetch

- arrow next/previous works
- swipe left/right works
- Today works
- cached week displays immediately
- rapid 5–10 swipes do not start 5–10 network requests
- after ~1.5 s settled, exactly one selected±1 refresh is queued
- consolidated `calendar.get_events` request is used
- if three-week window was refreshed recently, nav request is skipped
- no request starts while another HA/MET request is active

### Active-tab network behavior

- Calendar: only expected calendar refresh + expired-weather refresh
- Weather: only expired-weather refresh
- Chores: only chores refresh
- Alarm: only Alarmo refresh
- Settings: no recurring HA/MET traffic
- screen asleep: no active-tab recurring HTTP work

### Soak test

Leave each of these active for at least 10 minutes while monitoring serial:

1. Calendar
2. Weather
3. Chores
4. Alarm

Then repeatedly switch tabs and swipe weeks for another 10 minutes.

A release is not considered stable if there is any watchdog, SDIO assert, or stack protection fault.

---

## 26. Known bugs / quirks that are not the v1.7.0 target

- HA TLS currently defaults to encrypted-but-unverified (`setInsecure`) unless a CA PEM is configured.
- Wake-on-approach HA polling is intentionally disabled in the network-safe architecture; touch-to-wake remains.
- `camera_service` headers still contain legacy camera structures even though live feed functionality is removed.
- Some ArduinoJson calls previously produced deprecation warnings; warnings alone were not runtime failures.
- Some LVGL font glyph warnings came from curly punctuation.  Keep new UI text ASCII-safe.

Do not broaden v1.7.0 into cleanup of all of these unless required for the requested weather/prefetch work.

---

## 27. Reference behavior from ESPHome_WeatherDisplay

The referenced project is designed for the same 1280x800 class of panel and provides three pages.  The user's requested v1.7.0 should borrow only the useful interaction/layout ideas:

- a prominent current-conditions area
- metric tiles
- vertical daily forecast cards
- concise weather symbols and condition labels
- quick at-a-glance forecast

The reference daily page includes current temperature/humidity plus a six-day card forecast.  Its source API is Open-Meteo, but this project must use **MET Norway instead**.

Do not copy its YAML/source/assets verbatim.

---

## 28. Recommended implementation order

Follow this order to minimize debugging variables:

### Phase 1 — weather model/network only

- add weather config
- implement MET request with headers/cache semantics
- parse current/hourly/daily forecast
- print parsed summary to serial
- no UI yet
- soak test networking beside HA

### Phase 2 — Weather tab

- add Weather dashboard/footer tab
- render cached forecast
- add attribution
- verify theme switching and memory

### Phase 3 — Calendar weather indicators

- add icon/high-low per day
- render only from cached forecast
- no per-day HTTP

### Phase 4 — navigation-triggered selected±1 calendar refresh

- introduce common week navigation helper
- add 1.5 s debounce
- freshness check
- queue exactly one consolidated HA request after settled navigation

### Phase 5 — soak/stress test

- rapid swipes
- tab changes
- Weather + HA refresh near same time
- Wi-Fi reconnect
- idle 10+ minutes

Do not combine all phases and then debug a crash with no way to identify the responsible subsystem.

---

## 29. What success looks like

The finished v1.7.0 should feel like this:

- device boots into Calendar quickly
- current week events come from cache/HA as before
- each forecastable day has a subtle Outlook-like icon and high/low
- swiping to another week immediately changes the calendar from RAM if cached
- after the user stops navigating, the selected week ±1 refreshes once in the background
- Weather tab has a polished current-weather banner and six-day forecast
- weather remains cached and does not hammer MET Norway
- only one outbound HTTPS job runs at a time
- no watchdog resets, SDIO assertions, or stack faults

Stability is more important than maximum freshness.

---

## 30. Final warnings to the coding agent

1. **Do not guess hardware pins, display init, or C6 firmware.**  Those are already solved.
2. **Do not touch the known-good display/rotation pipeline** for unrelated UI bugs.
3. **Do not add concurrent HTTPS workers.**  ESP-Hosted has already proven sensitive.
4. **Do not re-add live camera support.**
5. **Do not copy unlicensed assets/code from the weather-display reference repo.**
6. **Do not overwrite `app_local.h` or `app_secrets.h`.**
7. **Do not claim a build passed without actually compiling it.**
8. **Do not emit partial code snippets as the final deliverable.**  Produce complete replacement files / an in-place update ZIP.
9. Preserve verbose serial diagnostics until v1.7.0 is proven stable.
10. When a crash occurs, inspect the exact log and task name before changing architecture again.

