#include "calendar_model.h"
#include "app_config.h"
#include "time_service.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/**
 * Colors
 * Green - 0x22C55E
 * Blue  - 0x3B82F6
 * Purple- 0xA855F7
 * Orange- 0xF59E0B
 * Red   - 0xEF4444
 * 
 */

const CalendarPerson CALENDAR_PEOPLE[4] = {
    {PERSON_1_NAME, 0x22C55E},
    {PERSON_2_NAME, 0x2596BE},
    {PERSON_3_NAME, 0xF06292},
    {PERSON_4_NAME, 0xBA68C8},
};

CalendarEvent CALENDAR_EVENTS[HA_MAX_EVENTS] = {};
size_t CALENDAR_EVENT_COUNT = 0;

namespace {

struct CalendarCacheSlot {
    bool valid;
    time_t week_start;
    uint32_t refreshed_ms;
    uint32_t access_serial;
    size_t count;
    CalendarEvent events[HA_MAX_EVENTS];
};

CalendarCacheSlot *g_cache = nullptr;
size_t g_cache_slots = 0;
uint32_t g_access_serial = 0;

void set_event(CalendarEvent &event,
               time_t start_epoch,
               uint16_t duration_min,
               uint8_t person,
               const char *title,
               const char *location) {
    memset(&event, 0, sizeof(event));
    event.start_epoch = start_epoch;
    event.end_epoch = start_epoch + static_cast<time_t>(duration_min) * 60;
    event.all_day = false;
    event.person = person;
    snprintf(event.title, sizeof(event.title), "%s", title ? title : "Event");
    snprintf(event.location, sizeof(event.location), "%s", location ? location : "");
    snprintf(event.source, sizeof(event.source), "Demo");
}

int compare_events(const void *a, const void *b) {
    const auto *ea = static_cast<const CalendarEvent *>(a);
    const auto *eb = static_cast<const CalendarEvent *>(b);
    if (ea->start_epoch < eb->start_epoch) return -1;
    if (ea->start_epoch > eb->start_epoch) return 1;
    if (ea->person < eb->person) return -1;
    if (ea->person > eb->person) return 1;
    return 0;
}

uint16_t event_minutes(const CalendarEvent &event) {
    struct tm local_tm = {};
    localtime_r(&event.start_epoch, &local_tm);
    return static_cast<uint16_t>(local_tm.tm_hour * 60 + local_tm.tm_min);
}

CalendarCacheSlot *find_slot(time_t week_start) {
    if (!g_cache) return nullptr;
    for (size_t i = 0; i < g_cache_slots; ++i) {
        if (g_cache[i].valid && g_cache[i].week_start == week_start) return &g_cache[i];
    }
    return nullptr;
}

CalendarCacheSlot *slot_for_store(time_t week_start) {
    if (!g_cache) return nullptr;
    if (CalendarCacheSlot *existing = find_slot(week_start)) return existing;

    for (size_t i = 0; i < g_cache_slots; ++i) {
        if (!g_cache[i].valid) return &g_cache[i];
    }

    CalendarCacheSlot *oldest = &g_cache[0];
    for (size_t i = 1; i < g_cache_slots; ++i) {
        if (g_cache[i].access_serial < oldest->access_serial) oldest = &g_cache[i];
    }
    return oldest;
}

} // namespace

bool calendar_cache_begin() {
    if (g_cache) return true;

    /*
     * A full multi-month cache is only a few hundred KB depending on compiler
     * padding, which is tiny relative to this board's 32 MB PSRAM. If a full allocation
     * ever fails, gracefully step down rather than consuming scarce internal RAM.
     */
    for (int slots = HA_CALENDAR_CACHE_WEEKS; slots >= 3; slots -= 2) {
        const size_t bytes = static_cast<size_t>(slots) * sizeof(CalendarCacheSlot);
        void *memory = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory) {
            g_cache = static_cast<CalendarCacheSlot *>(memory);
            g_cache_slots = static_cast<size_t>(slots);
            Serial0.printf("[Calendar] PSRAM week cache: %u slots, %u bytes\n",
                           static_cast<unsigned>(g_cache_slots),
                           static_cast<unsigned>(bytes));
            return true;
        }
    }

    Serial0.println("[Calendar] WARNING: PSRAM week cache allocation failed; using active week only");
    return false;
}

void calendar_model_load_demo() {
    CALENDAR_EVENT_COUNT = 0;
    time_t week = time_service_start_of_week_midnight(time_service_now());

    struct Demo {
        uint8_t day;
        uint16_t start_min;
        uint16_t duration;
        uint8_t person;
        const char *title;
        const char *location;
    };

    static const Demo demo[] = {
        {0,  9 * 60,      90, 2, "Soccer practice",   "Community field"},
        {0, 17 * 60 + 30, 60, 0, "Family dinner",     "Home"},
        {1,  8 * 60,      30, 0, "School drop-off",   "School"},
        {1, 12 * 60,      60, 1, "Lunch appointment", "Downtown"},
        {2, 15 * 60 + 30, 60, 3, "Music lesson",      "Music studio"},
        {2, 18 * 60,      45, 0, "Grocery pickup",    "Store"},
        {3,  8 * 60,      30, 0, "School drop-off",   "School"},
        {3, 17 * 60 + 30, 90, 2, "Team practice",     "Gym"},
        {4, 16 * 60,      60, 1, "Dentist",           "Clinic"},
        {4, 18 * 60 + 30, 60, 0, "Family dinner",     "Home"},
        {5, 15 * 60,     120, 3, "Friend visit",      "Home"},
        {6, 10 * 60,      90, 0, "House projects",    "Home"},
        {6, 18 * 60,     120, 0, "Movie night",       "Home"},
    };

    for (const Demo &d : demo) {
        if (CALENDAR_EVENT_COUNT >= HA_MAX_EVENTS) break;
        time_t day = time_service_add_days(week, d.day);
        struct tm tm_value = {};
        localtime_r(&day, &tm_value);
        tm_value.tm_hour = d.start_min / 60;
        tm_value.tm_min = d.start_min % 60;
        tm_value.tm_sec = 0;
        tm_value.tm_isdst = -1;
        set_event(CALENDAR_EVENTS[CALENDAR_EVENT_COUNT++], mktime(&tm_value),
                  d.duration, d.person, d.title, d.location);
    }
    calendar_model_sort(CALENDAR_EVENTS, CALENDAR_EVENT_COUNT);
}

void calendar_model_clear() {
    memset(CALENDAR_EVENTS, 0, sizeof(CALENDAR_EVENTS));
    CALENDAR_EVENT_COUNT = 0;
}

void calendar_model_replace(const CalendarEvent *events, size_t count) {
    if (!events) count = 0;
    if (count > HA_MAX_EVENTS) count = HA_MAX_EVENTS;
    if (count > 0) memcpy(CALENDAR_EVENTS, events, count * sizeof(CalendarEvent));
    if (count < HA_MAX_EVENTS) {
        memset(CALENDAR_EVENTS + count, 0, (HA_MAX_EVENTS - count) * sizeof(CalendarEvent));
    }
    CALENDAR_EVENT_COUNT = count;
    calendar_model_sort(CALENDAR_EVENTS, CALENDAR_EVENT_COUNT);
}

void calendar_model_sort(CalendarEvent *events, size_t count) {
    if (events && count > 1) qsort(events, count, sizeof(CalendarEvent), compare_events);
}

bool calendar_cache_store(time_t week_start,
                          const CalendarEvent *events,
                          size_t count,
                          uint32_t refreshed_ms) {
    if (!g_cache || g_cache_slots == 0) return false;
    CalendarCacheSlot *slot = slot_for_store(week_start);
    if (!slot) return false;

    if (!events) count = 0;
    if (count > HA_MAX_EVENTS) count = HA_MAX_EVENTS;
    slot->valid = true;
    slot->week_start = week_start;
    slot->refreshed_ms = refreshed_ms;
    slot->access_serial = ++g_access_serial;
    slot->count = count;
    if (count) memcpy(slot->events, events, count * sizeof(CalendarEvent));
    if (count < HA_MAX_EVENTS) {
        memset(slot->events + count, 0, (HA_MAX_EVENTS - count) * sizeof(CalendarEvent));
    }
    calendar_model_sort(slot->events, slot->count);
    return true;
}

bool calendar_cache_activate(time_t week_start) {
    CalendarCacheSlot *slot = find_slot(week_start);
    if (!slot) return false;
    slot->access_serial = ++g_access_serial;
    calendar_model_replace(slot->events, slot->count);
    return true;
}

bool calendar_cache_has(time_t week_start) {
    return find_slot(week_start) != nullptr;
}

bool calendar_cache_is_fresh(time_t week_start, uint32_t now_ms, uint32_t max_age_ms) {
    CalendarCacheSlot *slot = find_slot(week_start);
    if (!slot) return false;
    return static_cast<uint32_t>(now_ms - slot->refreshed_ms) < max_age_ms;
}

size_t calendar_cache_valid_weeks() {
    if (!g_cache) return 0;
    size_t count = 0;
    for (size_t i = 0; i < g_cache_slots; ++i) {
        if (g_cache[i].valid) ++count;
    }
    return count;
}

size_t calendar_cache_capacity_weeks() {
    return g_cache_slots;
}

bool calendar_event_is_on_day(const CalendarEvent &event, time_t day_value) {
    struct tm day_tm = {};
    localtime_r(&day_value, &day_tm);
    day_tm.tm_hour = 0;
    day_tm.tm_min = 0;
    day_tm.tm_sec = 0;
    day_tm.tm_isdst = -1;
    const time_t day_start = mktime(&day_tm);

    day_tm.tm_mday += 1;
    day_tm.tm_isdst = -1;
    const time_t day_end = mktime(&day_tm);

    const time_t event_end = event.end_epoch > event.start_epoch
                                 ? event.end_epoch
                                 : event.start_epoch + 1;
    return event.start_epoch < day_end && event_end > day_start;
}

size_t calendar_count_events_on_day(time_t day_value) {
    size_t count = 0;
    for (size_t i = 0; i < CALENDAR_EVENT_COUNT; ++i) {
        if (calendar_event_is_on_day(CALENDAR_EVENTS[i], day_value)) ++count;
    }
    return count;
}

const char *calendar_day_name(uint8_t day) {
    static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    return day < 7 ? names[day] : "";
}

void calendar_format_minutes(uint16_t minutes, char *buffer, size_t length) {
    int hour = (minutes / 60) % 24;
    const int minute = minutes % 60;
    const char *suffix = hour >= 12 ? "PM" : "AM";
    int display_hour = hour % 12;
    if (display_hour == 0) display_hour = 12;
    snprintf(buffer, length, "%d:%02d %s", display_hour, minute, suffix);
}

void calendar_format_event_time(const CalendarEvent &event, char *buffer, size_t length) {
    if (event.all_day) {
        snprintf(buffer, length, "ALL DAY");
        return;
    }
    calendar_format_minutes(event_minutes(event), buffer, length);
}

void calendar_format_event_range(const CalendarEvent &event, char *buffer, size_t length) {
    if (event.all_day) {
        snprintf(buffer, length, "All day");
        return;
    }

    struct tm start_tm = {};
    struct tm end_tm = {};
    localtime_r(&event.start_epoch, &start_tm);
    localtime_r(&event.end_epoch, &end_tm);
    char start[20];
    char end[20];
    calendar_format_minutes(static_cast<uint16_t>(start_tm.tm_hour * 60 + start_tm.tm_min), start, sizeof(start));
    calendar_format_minutes(static_cast<uint16_t>(end_tm.tm_hour * 60 + end_tm.tm_min), end, sizeof(end));
    snprintf(buffer, length, "%s - %s", start, end);
}
