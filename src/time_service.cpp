#include "time_service.h"
#include "app_config.h"
#include <Arduino.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static time_t g_base_epoch = 0;
static uint32_t g_base_millis = 0;
static constexpr time_t MIN_VALID_TIME = 1704067200; // 2024-01-01 UTC

static int month_number(const char *month) {
    static const char *names = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(names, month);
    return p ? static_cast<int>((p - names) / 3) : 0;
}

static time_t build_epoch() {
    char month[4] = {0};
    int day = 1;
    int year = 2026;
    int hour = 0;
    int minute = 0;
    int second = 0;

    sscanf(__DATE__, "%3s %d %d", month, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);

    struct tm tm_value = {};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month_number(month);
    tm_value.tm_mday = day;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;
    tm_value.tm_isdst = -1;
    return mktime(&tm_value);
}

void time_service_begin() {
    setenv("TZ", APP_TIMEZONE_POSIX, 1);
    tzset();

    const time_t system_now = time(nullptr);
    g_base_epoch = system_now >= MIN_VALID_TIME ? system_now : build_epoch();
    g_base_millis = millis();
}

time_t time_service_now() {
    /* Once SNTP updates libc time, prefer it automatically. */
    const time_t system_now = time(nullptr);
    if (system_now >= MIN_VALID_TIME) return system_now;
    return g_base_epoch + static_cast<time_t>((millis() - g_base_millis) / 1000UL);
}

bool time_service_has_network_time() {
    return time(nullptr) >= MIN_VALID_TIME;
}

void time_service_format_date(time_t value, char *buffer, size_t length) {
    static const char *weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    static const char *months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    struct tm local_tm = {};
    localtime_r(&value, &local_tm);
    snprintf(buffer, length, "%s, %s %d",
             weekdays[local_tm.tm_wday], months[local_tm.tm_mon], local_tm.tm_mday);
}

void time_service_format_time(time_t value, char *buffer, size_t length) {
    struct tm local_tm = {};
    localtime_r(&value, &local_tm);
    int hour = local_tm.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(buffer, length, "%d:%02d %s", hour, local_tm.tm_min,
             local_tm.tm_hour >= 12 ? "PM" : "AM");
}

void time_service_format_utc_iso(time_t value, char *buffer, size_t length) {
    struct tm utc_tm = {};
    gmtime_r(&value, &utc_tm);
    strftime(buffer, length, "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
}

time_t time_service_start_of_week(time_t value) {
    struct tm local_tm = {};
    localtime_r(&value, &local_tm);
    local_tm.tm_hour = 12; // noon avoids DST boundary problems for UI day arithmetic
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    local_tm.tm_mday -= local_tm.tm_wday; // Sunday
    local_tm.tm_isdst = -1;
    return mktime(&local_tm);
}

time_t time_service_start_of_week_midnight(time_t value) {
    struct tm local_tm = {};
    localtime_r(&value, &local_tm);
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    local_tm.tm_mday -= local_tm.tm_wday;
    local_tm.tm_isdst = -1;
    return mktime(&local_tm);
}

time_t time_service_add_days(time_t value, int days) {
    struct tm local_tm = {};
    localtime_r(&value, &local_tm);
    local_tm.tm_mday += days;
    local_tm.tm_isdst = -1;
    return mktime(&local_tm);
}
