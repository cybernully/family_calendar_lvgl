#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "app_config.h"

struct CalendarPerson {
    const char *name;
    uint32_t color;
};

struct CalendarEvent {
    time_t start_epoch;
    time_t end_epoch;
    bool all_day;
    uint8_t person;
    char title[80];
    char location[80];
    char description[256];
    char status[24];
    char source[48];
};

extern const CalendarPerson CALENDAR_PEOPLE[4];
extern CalendarEvent CALENDAR_EVENTS[HA_MAX_EVENTS];
extern size_t CALENDAR_EVENT_COUNT;

void calendar_model_load_demo();
void calendar_model_clear();
void calendar_model_replace(const CalendarEvent *events, size_t count);
void calendar_model_sort(CalendarEvent *events, size_t count);
bool calendar_event_is_on_day(const CalendarEvent &event, time_t day_value);
size_t calendar_count_events_on_day(time_t day_value);

/* Multi-week PSRAM cache. */
bool calendar_cache_begin();
bool calendar_cache_store(time_t week_start,
                          const CalendarEvent *events,
                          size_t count,
                          uint32_t refreshed_ms);
bool calendar_cache_activate(time_t week_start);
bool calendar_cache_has(time_t week_start);
bool calendar_cache_is_fresh(time_t week_start, uint32_t now_ms, uint32_t max_age_ms);
size_t calendar_cache_valid_weeks();
size_t calendar_cache_capacity_weeks();

const char *calendar_day_name(uint8_t day);
void calendar_format_minutes(uint16_t minutes, char *buffer, size_t length);
void calendar_format_event_time(const CalendarEvent &event, char *buffer, size_t length);
void calendar_format_event_range(const CalendarEvent &event, char *buffer, size_t length);
