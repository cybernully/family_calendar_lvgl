# Family Calendar LVGL 1.7.0

## 1.7.0 - Weather tab, calendar weather strips, and safe week prefetch

Version 1.7.0 adds a dedicated Weather dashboard and restores automatic
selected-week prefetch in a network-safe way for ESP-Hosted stability.

- New footer tab order:

```text
Calendar   Chores   Meals   Weather   Alarm   Settings
```

- Weather source is Home Assistant (`weather.*` entity) fetched through HA APIs
- Weather refreshes are rate-limited and cached in RAM for both Weather and Calendar displays
- Weather data is cached and reused by both Weather and Calendar screens
- Calendar day headers now include compact weather icon + high/low when forecast data exists
- Week navigation now behaves as:
  - immediate cached week switch
  - debounce period
  - one consolidated Home Assistant `calendar.get_events` request for selected week plus +/-1
  - skip network request when cached +/-1 weeks are still fresh

Weather configuration is local-only and should be added to `include/app_local.h`:

```cpp
#define HA_WEATHER_ENTITY "weather.forecast_home"
// #define WEATHER_USE_IMPERIAL 1
```

If weather is not configured, the Weather tab shows guidance and no request is sent.


## 1.6.1 - Reliable distance-based calendar swipe

Version 1.6.1 replaces the LVGL `LV_EVENT_GESTURE` implementation added in
1.6.0 with direct touch-distance tracking.  The previous implementation
depended on LVGL's gesture velocity threshold, which could reject a normal or
slow finger drag on this touch controller.

- Swipe left at least 70 px across the seven-day calendar area to move forward
  one week.
- Swipe right at least 70 px to move back one week.
- No minimum swipe speed is required.
- Strongly vertical drags are ignored.
- Swiping remains cache-only and does not itself force a Home Assistant request.
- Serial diagnostics now include measured `dx`/`dy` whenever a swipe is accepted.



## 1.6.0 - Calendar swipe + header status icons

Version 1.6.0 keeps the 1.5.9 active-tab refresh policy and adds three
touch/UI improvements:

- Swipe **left** anywhere across the seven-day calendar week to move to the
  next week; swipe **right** to move to the previous week.  Swipes remain
  cache-only, exactly like the Previous/Next buttons, and do not themselves
  force a Home Assistant request.
- The header now includes a Wi-Fi status chip with explicit **ON/OFF** state
  and four signal-strength bars based on RSSI.
- The light/dark toggle now uses drawn sun/moon graphics instead of the prior
  eye symbol.  The icons are LVGL primitives, so they do not depend on Unicode
  glyph support.

## 1.5.9 - Active-tab auto refresh

Version 1.5.9 keeps the 1.5.8 network-safe architecture but restores useful
automatic updates in a controlled way: **only the dashboard currently visible
on the panel is eligible to refresh.**  Hidden tabs generate no periodic Home
Assistant traffic, and automatic refresh pauses entirely while the display is
asleep.

Refresh cadence:

- **Calendar / Meals:** every 5 minutes while visible.
- **Chores:** every 1 minute while visible.
- **Alarmo:** every 15 seconds while visible.
- **Settings:** local status refresh only; no recurring HA request.

A 1.5-second settle window after changing tabs or waking the display prevents a
tab tap from immediately firing an HTTPS request.  Calendar week navigation is
still RAM/cache-only; an automatic Calendar/Meals refresh updates the selected
week plus one week before and after.  Manual Refresh buttons remain available.
The three automatic intervals can also be overridden in `include/app_local.h`
without being overwritten by future in-place updates.

## 1.5.8 - ESP-Hosted network-safe mode

Version 1.5.8 responds to watchdog resets that still occurred in 1.5.7 even
when calendar navigation itself generated no network traffic.  The remaining
automatic Home Assistant jobs (chores / Alarmo, and the optional wake sensor)
could still enter the same ESP-Hosted HTTPS stall and starve CPU0's idle task.

Changes in 1.5.8:

- Calendar navigation remains completely RAM/cache-only.
- The one boot calendar load is delayed until Wi-Fi has remained continuously
  connected for 10 seconds.
- The boot calendar load now uses one Home Assistant `calendar.get_events`
  request for all configured calendars instead of four separate calendar HTTP
  requests.
- Periodic chore polling is disabled.  Chores refresh on explicit **Refresh**
  and after a chore action.
- Periodic Alarmo polling is disabled.  Alarm state refreshes when the Alarm
  dashboard asks for it, on explicit **Refresh**, and after an alarm action.
- Home Assistant wake-sensor polling is disabled in this network-safe build.
  Screen timeout and touch-to-wake remain fully enabled.
- Wi-Fi power save is disabled and automatic reconnect is enabled because the
  panel is mains powered.
- The temporary shared HTTP serialization mutex added for camera streaming has
  been removed now that camera networking is gone.
- Additional request-start/end logging identifies the exact HA operation if a
  future transport stall occurs.

This intentionally favors stability over continuous background freshness.
Home Assistant data can still be refreshed from the relevant dashboard, but
the panel no longer creates hidden recurring HTTPS traffic while sitting idle.

## 1.5.7 - Calendar stability mode

Version 1.5.7 removes the remaining automatic calendar-network activity that
was able to starve the ESP32-P4 idle task through ESP-Hosted.  The boot sync
still loads the current week plus one week before and after, and the PSRAM LRU
cache still preserves previously loaded weeks.  After boot, week navigation is
strictly cache-only.  An uncached week is loaded only when **Refresh now** is
pressed.  The one-minute automatic calendar refresh is disabled.

Chores and Alarmo background polling was still present in 1.5.7 and was removed in 1.5.8.  Live
camera feeds remain removed.  The harmless first-boot NVS `wake_sensor NOT_FOUND`
log has also been suppressed by checking for the key before reading it.

## 1.5.6 - Home Assistant watchdog hardening

Version 1.5.6 addresses a task-watchdog reset observed while the background
Home Assistant worker was prefetching a three-week calendar window.  The HA
worker now runs at FreeRTOS idle priority, uses watchdog-safe local-network HTTP/TLS
timeouts, yields between requests, cancels stale prefetches when the selected
week changes, and debounces rapid week navigation so multiple taps coalesce
into a single background request.  Live camera feeds remain removed; the
lightweight HA wake-sensor feature remains available.


## 1.5.5 - Camera feed rollback / stability release

Version 1.5.5 removes live camera viewing from the ESP32-P4 panel.  The camera
feed experiment in 1.5.0-1.5.4 added too much runtime pressure to this hardware
and caused repeated watchdog/stack/transport failures.  This release returns the
application to the stable dashboard architecture while retaining the useful
screen timeout and approach-wake features.

### Dashboard navigation

The footer is now:

```text
Calendar   Chores   Meals   Alarm   Settings
```

Calendar remains the default dashboard after every reboot.  Icon-based footer
navigation and the icon-only light/dark toggle remain unchanged.

### What was removed

- The Cameras dashboard.
- Home Assistant `camera.*` discovery.
- Home Assistant camera proxy JPEG downloads.
- JPEG frame buffers and image double buffering in PSRAM.
- JPEG decoding and the JPEGDEC library dependency.
- The dedicated live-camera worker and camera refresh scheduling.

This means the panel no longer requests or decodes camera images at all.

### Screen timeout and wake behavior remain

The screen timeout remains configurable from **Settings**.  When the timeout
expires, only the display backlight is turned off; the ESP32-P4, Wi-Fi, and
cached data remain available.  Active-tab automatic Home Assistant refreshes
pause while the display is asleep and resume after it wakes.

A touch wakes the display, and the wake touch is consumed so it does not
accidentally activate a control.

In 1.5.9 network-safe mode, outbound Home Assistant wake-sensor polling is
disabled.  Touch-to-wake remains active.  A future wake-on-approach design
should use a local sensor or a push/event path rather than one-second HTTPS
polling.

### Calendar

- Calendar is the boot/default dashboard.
- Home Assistant calendar synchronization runs on a background FreeRTOS task.
- Boot, manual refresh, and active-tab refresh load the selected week plus one week before and one week after.
- Previously visited weeks remain in the PSRAM LRU cache.
- Tapping a calendar event opens its date/time, calendar, location, status,
  Home Assistant source, and description from cached RAM data.
- Calendar navigation is cache-only and never starts a request by itself.
- While Calendar or Meals is visible, calendar data auto-refreshes every 5 minutes.
- Use **Refresh now** to load an uncached week or refresh the selected week immediately.

### Chores

- Uses Home Assistant `todo.*` entities.
- Selectable chore list stored in Preferences.
- Tap chores to mark complete/incomplete in Home Assistant.
- Auto-refreshes every 1 minute only while the Chores tab is visible.
- Reset-all and manual refresh remain available.

### Alarmo

The Alarm dashboard supports the selected Alarmo `alarm_control_panel` entity,
including supported arm modes, disarm, transition status, open sensors, and
code entry when required.  State auto-refreshes every 15 seconds only while
the Alarm tab is visible, and also refreshes on demand and after commands.

## Persistent local files

In-place update ZIPs intentionally do not include these files:

```text
include/app_secrets.h
include/app_local.h
```

Your Wi-Fi credentials, Home Assistant token/base URL, calendar mappings, and
household-specific settings remain intact when extracting an update over the
existing project.

## Build / upload

From the existing project directory:

```bash
pio run -t clean
pio run -t upload
pio device monitor -b 115200
```

The project remains pinned to Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4 and the
known-good JC8012P4A1C SKU 10153001 (2624) JD9365 display profile.