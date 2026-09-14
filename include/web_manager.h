#pragma once

#include <stddef.h>

/* Lightweight local web management and browser-based OTA firmware updates. */
void web_manager_begin();
void web_manager_loop();

/* Maintenance suppresses new active-tab HA refreshes while existing work drains. */
bool web_manager_maintenance_active();

/* True only while firmware bytes are being written to the inactive OTA slot. */
bool web_manager_ota_in_progress();

/*
 * Copy the configured hybrid weather sources into caller-owned buffers.
 * The current/daily source is normally a local PWS entity; the hourly source
 * can be a different Home Assistant weather provider.
 */
void web_manager_get_weather_sources(char *current_daily, size_t current_daily_len,
                                     char *hourly, size_t hourly_len);
