#pragma once

/* Copy/rename to app_local.h and customize.  In-place updates preserve app_local.h. */
#define PERSON_1_NAME "Adult 1"
#define PERSON_2_NAME "Adult 2"
#define PERSON_3_NAME "Family"
#define PERSON_4_NAME "Other"

/* Optional compile-time color defaults; Web Management can override these. */
#define PERSON_1_COLOR 0x22C55EUL
#define PERSON_2_COLOR 0x2596BEUL
#define PERSON_3_COLOR 0xF06292UL
#define PERSON_4_COLOR 0xBA68C8UL

#define HA_CALENDAR_1 "calendar.person_1"
#define HA_CALENDAR_2 "calendar.person_2"
#define HA_CALENDAR_3 "calendar.family"
#define HA_CALENDAR_4 ""

/*
 * Optional: preselect a Home Assistant To-do list for the Chores dashboard.
 * Leave blank to choose an available todo.* entity directly on the touchscreen.
 */
#define HA_CHORE_TODO_ENTITY ""

/* Optional: preselect an Alarmo alarm_control_panel entity.
 * Leave blank to choose an available Alarmo panel on the touchscreen. */
#define HA_ALARMO_ENTITY ""

/*
 * Optional: preselect a Home Assistant binary_sensor for wake-on-approach.
 * Good choices are a camera person-detection, motion, occupancy, or presence
 * binary sensor. Leave blank to choose one from the Settings screen.
 */
#define HA_WAKE_PRESENCE_ENTITY ""

/*
 * Optional default screen timeout in seconds. Supported Settings choices are
 * Off (0), 30, 60, 120, 300, and 600 seconds. The on-screen choice persists.
 */
#define APP_SCREEN_TIMEOUT_SECONDS 120U

/* Optional local web-management defaults.  Once changed from the web UI, the
 * NVS values take precedence and survive future firmware updates. */
// #define WEB_MANAGER_HOSTNAME "family-calendar"
// #define WEB_MANAGER_DEFAULT_USER "admin"
// #define WEB_MANAGER_DEFAULT_PASSWORD "choose-a-strong-password"

/* Legacy/local chore labels retained for compatibility. */
#define CHORE_1_NAME "Feed pets"
#define CHORE_2_NAME "Dishwasher"
#define CHORE_3_NAME "Trash / recycling"
#define CHORE_4_NAME "Tidy kitchen"
#define CHORE_5_NAME "Laundry"
#define CHORE_6_NAME "Mail / packages"

/* Optional active-tab auto-refresh overrides (milliseconds).
 * Uncomment only if you want different refresh cadences.
 */
// #define HA_ACTIVE_TAB_CALENDAR_REFRESH_MS 300000UL
// #define HA_ACTIVE_TAB_CHORES_REFRESH_MS 60000UL
// #define HA_ACTIVE_TAB_ALARM_REFRESH_MS 15000UL

/*
 * Weather source defaults.  v2.2.1 can combine two Home Assistant weather
 * entities in one snapshot request:
 *   - CURRENT_DAILY supplies the live PWS/current conditions and daily forecast
 *   - HOURLY supplies the next-hour forecast series
 *
 * These are only defaults.  Both values can also be changed from the web
 * manager and are then persisted in NVS across firmware updates.
 */
#define HA_WEATHER_CURRENT_DAILY_ENTITY "weather.forecast_home"
#define HA_WEATHER_HOURLY_ENTITY "weather.forecast_home"

/* Legacy single-source alias retained for older local configurations. */
// #define HA_WEATHER_ENTITY "weather.forecast_home"

/* Optional units override: 1 = F/mph/in, 0 = C/mps/mm. */
// #define WEATHER_USE_IMPERIAL 1
