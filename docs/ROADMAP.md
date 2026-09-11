# Family Calendar roadmap

## 1.1 - Home Assistant calendar connectivity
- ESP-Hosted Wi-Fi through onboard ESP32-C6
- Matching ESP-Hosted 2.12.3 firmware on P4/C6
- NTP
- HTTPS Home Assistant REST authentication
- Calendar discovery and mapped calendar entities
- Week polling/navigation sync

## 1.2 - Dashboard foundation
- Calendar remains the default dashboard at every boot
- One-minute Home Assistant calendar refresh
- Persistent light/dark mode
- Persistent display brightness
- Reworked label sizing and calendar layout to eliminate clipped small text
- Restored translucent event-detail overlay using the single persistent LVGL framebuffer path
- Up to six visible events/day plus a `+N more` indicator
- Multi-day events shown on every day they overlap
- Chores dashboard with tap-to-complete daily persistent checklist
- Meals dashboard inferred from meal-like Home Assistant calendar events
- Settings dashboard with Wi-Fi, HA, calendar, NTP, uptime and display status
- Manual Home Assistant sync controls

## 1.2.1 - Physical-panel layout correction
- Reset inherited LVGL theme padding on pixel-positioned containers and buttons
- Remove padding from auto-sized labels to prevent partial glyph clipping
- Taller two-row header and recalculated calendar content area
- Wider calendar sidebar and cleaner event/day spacing
- Full-width footer tab layout with larger touch targets

## Next
- Configure chores from Home Assistant todo entities
- Shopping list dashboard
- Direct meal planning/editing instead of keyword inference
- Presence / Alarmo / home controls
- On-device configuration for HA entity mapping
- Proper TLS CA validation
- OTA application firmware updates
- Persistent offline calendar cache

## 1.3.0 - Responsive background Home Assistant sync

- Moved HA discovery, HTTPS GETs, and JSON parsing off the LVGL/main loop onto a FreeRTOS worker task.
- Added RAM double buffering: the UI continues reading the active event snapshot while the worker builds the next snapshot.
- Commit the completed event set from the main loop only after parsing finishes.
- Removed the redundant Calendar toolbar Sync button; sidebar Refresh now remains.
- Kept the one-minute automatic refresh cadence.


## 1.3.0 delivered

- 16-week PSRAM LRU calendar cache.
- Background three-week prefetch centered on selected week.
- Home Assistant todo.* chore list discovery, selection, read, and completion updates.
- Chore updates are optimistic in the UI and queued to the HA worker.

## 1.4.0 - Alarmo dashboard

- Added a fifth **Alarm** dashboard while keeping Calendar as the default at boot.
- Discover and persist a selected Alarmo `alarm_control_panel` entity on-device.
- Background state polling through the existing Home Assistant worker.
- Display current state, next state/delay, open sensors, and last-triggered information.
- Arm Home, Away, Night, and Vacation based on Home Assistant supported-feature flags.
- Disarm with optional Alarmo code entry.
- On-screen numeric/text password keyboard driven by Home Assistant `code_format`.
- Alarm codes are never persisted and are cleared after the command is queued.
- Safe arming path uses `force: false`; the display never bypasses Alarmo open-sensor validation.
- Skip-exit-delay control is only exposed during `arming`, not during `pending` entry delay.

## 1.4.1 - Calendar event details

- Tapping any calendar event opens a large scrollable details overlay.
- Details include title, full date/time span, mapped calendar/person, location, Home Assistant source entity, status, and description.
- Description/status are retained with each cached event in PSRAM, so opening details never requires another Home Assistant request.
- The same details overlay is used when tapping meal events sourced from the calendar.

## 1.5.1 - Cameras, Settings, and display wake

- Added a sixth **Cameras** dashboard with Home Assistant `camera.*` discovery.
- 2x2 JPEG camera grid with tap-to-open near-live large view.
- Dedicated camera FreeRTOS worker and PSRAM double buffers keep network/JPEG work off the LVGL loop.
- Added icons to all footer dashboard buttons.
- Renamed **Home** dashboard to **Settings**.
- Replaced the text light/dark button with an icon-only theme button.
- Added persistent screen timeout choices and backlight blanking.
- First touch wakes the display without activating the touched control.
- Added camera/person-based wake through a selectable Home Assistant motion/occupancy/presence/person `binary_sensor`.
- Local onboard MIPI-CSI camera wake remains a future low-level driver task; 1.5.1 does not disturb the known-good display/touch stack.


## 1.5.3 - Camera watchdog hardening

- Replace full `/api/states` camera discovery with compact `/api/template` discovery.
- Run camera worker at priority 0 and explicitly yield during JPEG transfer/decode.
- Preserve camera dashboard, screen timeout, wake sensor, icons, Alarmo, chores, and cached calendar behavior.

## 1.5.4 - Camera stack-overflow hardening

- Move the large JPEGDEC workspace off the FreeRTOS task stack and allocate it lazily in PSRAM.
- Move camera/wake discovery staging arrays from task stack to PSRAM.
- Add camera worker stack high-water diagnostics around first JPEG decode.
- Keep camera HTTP 500 failures isolated to the affected camera tile.
- Replace an unsupported em-dash UI glyph with an ASCII hyphen.

## 1.5.5 - Camera feed rollback / stability

- Removed the Cameras dashboard and all Home Assistant camera JPEG fetching.
- Removed JPEGDEC and all camera image/frame-buffer allocations.
- Returned footer navigation to Calendar / Chores / Meals / Alarm / Settings.
- Retained screen timeout, touch-to-wake, and lightweight Home Assistant
  person/motion/occupancy/presence binary-sensor wake.
- Wake-sensor discovery remains compact through `/api/template`; active wake
  state polling uses only `/api/states/<selected binary_sensor>`.
- No live camera data is requested by the panel in this release.


## 1.5.6 - HA watchdog hardening

- Run the Home Assistant worker at idle priority so ESP-Hosted/TLS work cannot
  starve the FreeRTOS idle task.
- Keep HTTP and connect timeouts below the task-watchdog window.
- Yield between calendar requests and major worker operations.
- Debounce rapid week navigation and cancel stale prefetch windows.
- Keep live camera feeds removed while retaining lightweight wake-by-HA-sensor.


## 1.5.7 - Calendar network stability mode

- Disable automatic calendar prefetch and periodic calendar refresh.
- Keep navigation cache-only and require Refresh now for uncached weeks.
- Retain the initial boot three-week calendar load.


## 1.6.0 - Calendar swipe and header indicators

- Horizontal swipe over the calendar week changes weeks: left = next, right = previous.
- Header Wi-Fi indicator shows ON/OFF plus four RSSI bars.
- Theme toggle uses LVGL-drawn sun/moon graphics with no Unicode dependency.
- Retains the 1.5.9 active-tab-only Home Assistant refresh policy.

## 1.5.9 - Active-tab auto refresh

- Preserve 1.5.8 single-worker ESP-Hosted safety model.
- Only the visible dashboard may schedule recurring Home Assistant traffic.
- Calendar/Meals refresh every 5 minutes while visible.
- Chores refresh every minute while visible.
- Alarmo refreshes every 15 seconds while visible.
- Settings remains local-only.
- Pause all automatic HA refresh while the display is asleep.
- Keep calendar week navigation RAM-only and retain manual Refresh controls.

## 1.5.8 - ESP-Hosted network-safe mode

- Disable hidden recurring Home Assistant HTTPS requests while the panel is idle.
- Delay the one boot calendar load until Wi-Fi has been continuously connected for 10 seconds.
- Replace four per-calendar REST GETs with one `calendar.get_events` service request for all configured calendars.
- Disable periodic Chores and Alarmo polling; retain explicit refreshes and action-driven follow-up reads.
- Disable outbound HA presence/wake polling; touch-to-wake remains local.
- Disable Wi-Fi power save and enable automatic reconnect on the ESP-Hosted C6.
- Remove the camera-era shared HTTP serialization mutex.
- Add request start/end diagnostics for any remaining HA transport stalls.
