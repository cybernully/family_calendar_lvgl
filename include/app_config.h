#pragma once

#define APP_NAME "Family Hub"
#define APP_VERSION "2.2.1"

/* JC8012P4A1C native panel is portrait; 90 degrees gives 1280x800 landscape. */
#define APP_DISPLAY_ROTATION 90
#define APP_DEFAULT_BACKLIGHT 80

/* Central Time with automatic US daylight-saving rules. */
#define APP_TIMEZONE_POSIX "CST6CDT,M3.2.0,M11.1.0"

/*
 * Household-specific names and Home Assistant calendar mappings live in
 * include/app_local.h.  That file is intentionally kept separate from the
 * versioned application configuration so future in-place updates do not
 * overwrite the user's local calendar mappings.
 */
#if __has_include("app_local.h")
#include "app_local.h"
#endif

/*
 * v2.2.x local web management / OTA defaults.  These can be overridden in
 * app_local.h.  The web UI stores hostname/admin/UI-setting overrides in NVS,
 * so firmware updates do not erase them.
 */
#ifndef WEB_MANAGER_HOSTNAME
#define WEB_MANAGER_HOSTNAME "family-calendar"
#endif
#ifndef WEB_MANAGER_PORT
#define WEB_MANAGER_PORT 80
#endif
#ifndef WEB_MANAGER_DEFAULT_USER
#define WEB_MANAGER_DEFAULT_USER "admin"
#endif
#ifndef WEB_MANAGER_DEFAULT_PASSWORD
#define WEB_MANAGER_DEFAULT_PASSWORD "familyhub"
#endif

#ifndef PERSON_1_NAME
#define PERSON_1_NAME "Adult 1"
#endif
#ifndef PERSON_2_NAME
#define PERSON_2_NAME "Adult 2"
#endif
#ifndef PERSON_3_NAME
#define PERSON_3_NAME "Child 1"
#endif
#ifndef PERSON_4_NAME
#define PERSON_4_NAME "Child 2"
#endif

#ifndef HA_CALENDAR_1
#define HA_CALENDAR_1 ""
#endif
#ifndef HA_CALENDAR_2
#define HA_CALENDAR_2 ""
#endif
#ifndef HA_CALENDAR_3
#define HA_CALENDAR_3 ""
#endif
#ifndef HA_CALENDAR_4
#define HA_CALENDAR_4 ""
#endif


/*
 * Local chore defaults.  Any of these can be overridden in include/app_local.h
 * without being overwritten by future in-place updates.
 */
#ifndef CHORE_1_NAME
#define CHORE_1_NAME "Feed pets"
#endif
#ifndef CHORE_2_NAME
#define CHORE_2_NAME "Dishwasher"
#endif
#ifndef CHORE_3_NAME
#define CHORE_3_NAME "Trash / recycling"
#endif
#ifndef CHORE_4_NAME
#define CHORE_4_NAME "Tidy kitchen"
#endif
#ifndef CHORE_5_NAME
#define CHORE_5_NAME "Laundry"
#endif
#ifndef CHORE_6_NAME
#define CHORE_6_NAME "Mail / packages"
#endif

#ifndef APP_DEFAULT_DARK_MODE
#define APP_DEFAULT_DARK_MODE 0
#endif

/*
 * Network-safe active-tab refresh policy.
 *
 * Week navigation itself remains RAM-only.  The UI periodically refreshes only
 * the dashboard that is actually visible, and pauses those automatic Home
 * Assistant requests whenever the display is asleep.  This keeps background
 * ESP-Hosted traffic low while still keeping the screen a user is looking at
 * reasonably fresh.
 */
#define HA_SYNC_INTERVAL_MS 0UL
#define HA_RETRY_INTERVAL_MS 10000UL
#ifndef HA_ACTIVE_TAB_SETTLE_MS
#define HA_ACTIVE_TAB_SETTLE_MS 1500UL
#endif
#ifndef HA_ACTIVE_TAB_CALENDAR_REFRESH_MS
#define HA_ACTIVE_TAB_CALENDAR_REFRESH_MS 300000UL  /* Calendar + Meals: 5 min */
#endif
#ifndef HA_ACTIVE_TAB_CHORES_REFRESH_MS
#define HA_ACTIVE_TAB_CHORES_REFRESH_MS 60000UL     /* Chores: 1 min */
#endif
#ifndef HA_ACTIVE_TAB_ALARM_REFRESH_MS
#define HA_ACTIVE_TAB_ALARM_REFRESH_MS 1000UL      /* Alarmo: 1 sec */
#endif

#ifndef WEATHER_ACTIVE_TAB_REFRESH_MS
#define WEATHER_ACTIVE_TAB_REFRESH_MS 1800000UL     /* Weather: 30 min fallback */
#endif

/*
 * Keep a single slow HA/TLS operation below the task-watchdog window.  The
 * ESP-Hosted SDIO transport can occasionally stall until the HTTP timeout;
 * an 8 second timeout was long enough for IDLE0 to miss its watchdog reset.
 */
#define HA_HTTP_CONNECT_TIMEOUT_MS 2000
#define HA_HTTP_TIMEOUT_MS 2500
#define HA_HTTP_SERIALIZE_TIMEOUT_MS 10000UL
#define HA_HTTP_INTER_REQUEST_GAP_MS 1000UL

/*
 * HA HTTP/JSON work runs in the background at idle priority.  This is
 * intentional on ESP32-P4 + ESP-Hosted: networking must never outrank the
 * FreeRTOS idle task for several seconds at a time.
 */
#define HA_SYNC_TASK_STACK_BYTES 16384
#define HA_SYNC_TASK_PRIORITY 0

/* Debounced selected-week +/-1 refresh after week navigation settles. */
#ifndef HA_CALENDAR_NAV_DEBOUNCE_MS
#define HA_CALENDAR_NAV_DEBOUNCE_MS 1500UL
#endif
#ifndef HA_CALENDAR_NAV_REFRESH_MAX_AGE_MS
#define HA_CALENDAR_NAV_REFRESH_MAX_AGE_MS 300000UL
#endif

/* Wait for Wi-Fi to remain continuously connected before the single boot calendar load. */
#define HA_BOOT_NETWORK_STABLE_MS 10000UL

/* Maximum events retained per cached calendar week. */
#define HA_MAX_EVENTS 64

/*
 * Keep multiple previously visited weeks in PSRAM.  Each Home Assistant sync
 * fetches the selected week plus one week before and one week after, then
 * stores all three snapshots in this LRU cache.
 */
#define HA_PREFETCH_RADIUS_WEEKS 1
#define HA_CALENDAR_CACHE_WEEKS 16

/* Home Assistant To-do / Chores integration. */
#ifndef HA_CHORE_TODO_ENTITY
#define HA_CHORE_TODO_ENTITY ""
#endif
#define HA_MAX_CHORES 24
#define HA_MAX_TODO_LISTS 10
#define HA_CHORE_SYNC_INTERVAL_MS 0UL  /* scheduled by active-tab UI policy */
#define HA_CHORE_ACTION_QUEUE_SIZE 32


/* Screen timeout / wake behavior.  0 disables automatic blanking. */
#ifndef APP_SCREEN_TIMEOUT_SECONDS
#define APP_SCREEN_TIMEOUT_SECONDS 120U
#endif
#define APP_SCREEN_TIMEOUT_MIN_SECONDS 30U
#define APP_SCREEN_TIMEOUT_MAX_SECONDS 600U

/*
 * Optional Home Assistant binary_sensor used for proximity wake.  Leave blank
 * to choose a motion/occupancy/presence/person sensor from the Settings screen.
 * This is intentionally separate from the onboard MIPI-CSI camera driver.
 */
#ifndef HA_WAKE_PRESENCE_ENTITY
#define HA_WAKE_PRESENCE_ENTITY ""
#endif
#define HA_MAX_WAKE_SENSORS 20
#define HA_PRESENCE_SYNC_INTERVAL_MS 0UL  /* outbound wake polling remains disabled */
#define HA_WAKE_TASK_STACK_BYTES 12288
#define HA_WAKE_TASK_PRIORITY 0

/* Home Assistant Alarmo integration.  Leave blank to choose the Alarmo panel on-screen. */
#ifndef HA_ALARMO_ENTITY
#define HA_ALARMO_ENTITY ""
#endif
#define HA_MAX_ALARM_PANELS 8
#define HA_MAX_ACTIVE_ALARM_SENSORS 24
#define HA_ALARM_ACTION_QUEUE_SIZE 4
#define HA_ALARM_SYNC_INTERVAL_MS 0UL  /* scheduled by active-tab UI policy */
#define HA_ALARM_TRANSITION_SYNC_INTERVAL_MS 0UL

/*
 * This Family Calendar installation connects to an HTTPS-only Home Assistant.
 * Arduino does not have a system CA store configured for this project, so the
 * default below permits encrypted HTTPS without certificate verification.
 *
 * For full certificate validation later, define HA_TLS_CA_CERT_PEM in
 * app_secrets.h with the PEM root/intermediate certificate and set this to 0.
 */
#ifndef HA_TLS_ALLOW_INSECURE
#define HA_TLS_ALLOW_INSECURE 1
#endif

/*
 * JC8012P4A1C boards can ship with an older ESP32-C6 ESP-Hosted firmware than
 * Arduino-ESP32 3.3.8 expects.  Version 1.1.3 embeds the official matching
 * 2.12.3 co-processor image at build time and upgrades an older C6 before
 * attempting Wi-Fi association.  It never downgrades a newer C6 firmware.
 */
#define HOSTED_C6_AUTO_UPDATE 1

/*
 * Weather source defaults.  v2.2.1 supports a hybrid Home Assistant snapshot:
 * one weather entity supplies current conditions + daily forecast while a
 * second entity supplies the hourly forecast.  Existing installations that
 * only define HA_WEATHER_ENTITY remain compatible because both hybrid sources
 * fall back to it until overridden from app_local.h or the web manager.
 */
#ifndef HA_WEATHER_ENTITY
#define HA_WEATHER_ENTITY ""
#endif
#ifndef HA_WEATHER_CURRENT_DAILY_ENTITY
#define HA_WEATHER_CURRENT_DAILY_ENTITY HA_WEATHER_ENTITY
#endif
#ifndef HA_WEATHER_HOURLY_ENTITY
#define HA_WEATHER_HOURLY_ENTITY HA_WEATHER_ENTITY
#endif
#ifndef WEATHER_USE_IMPERIAL
#define WEATHER_USE_IMPERIAL 1
#endif

#ifndef WEATHER_HTTP_CONNECT_TIMEOUT_MS
#define WEATHER_HTTP_CONNECT_TIMEOUT_MS 2000
#endif
#ifndef WEATHER_HTTP_TIMEOUT_MS
#define WEATHER_HTTP_TIMEOUT_MS 2500
#endif
#ifndef WEATHER_FALLBACK_REFRESH_MS
#define WEATHER_FALLBACK_REFRESH_MS 1800000UL
#endif
#ifndef WEATHER_RETRY_INTERVAL_MS
#define WEATHER_RETRY_INTERVAL_MS 300000UL
#endif
#ifndef WEATHER_MIN_REQUEST_GAP_MS
#define WEATHER_MIN_REQUEST_GAP_MS 60000UL
#endif

#ifndef WEATHER_MAX_HOURLY_POINTS
#define WEATHER_MAX_HOURLY_POINTS 18
#endif
#ifndef WEATHER_MAX_DAILY_POINTS
#define WEATHER_MAX_DAILY_POINTS 9
#endif
