#pragma once

#include <time.h>
#include <stddef.h>

void time_service_begin();
time_t time_service_now();
bool time_service_has_network_time();
void time_service_format_date(time_t value, char *buffer, size_t length);
void time_service_format_time(time_t value, char *buffer, size_t length);
void time_service_format_utc_iso(time_t value, char *buffer, size_t length);
time_t time_service_start_of_week(time_t value);
time_t time_service_start_of_week_midnight(time_t value);
time_t time_service_add_days(time_t value, int days);
