#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "app_config.h"

enum class WeatherCondition : uint8_t {
    Unknown = 0,
    Clear,
    PartlyCloudy,
    Cloudy,
    Rain,
    Showers,
    Thunderstorm,
    Snow,
    Fog,
};

struct WeatherCurrent {
    bool valid;
    time_t epoch;
    WeatherCondition condition;
    char symbol_code[40];
    float temperature_c;
    float today_high_c;
    float today_low_c;
    int humidity_pct;
    float pressure_hpa;
    float wind_mps;
    float wind_direction_deg;
    float precipitation_mm;
    int precipitation_probability_pct;
};

struct WeatherHourForecast {
    bool valid;
    time_t epoch;
    WeatherCondition condition;
    char symbol_code[40];
    float temperature_c;
    float precipitation_mm;
    int precipitation_probability_pct;
    float wind_mps;
};

struct WeatherDayForecast {
    bool valid;
    time_t local_day_epoch;
    WeatherCondition condition;
    char symbol_code[40];
    float high_c;
    float low_c;
    float precipitation_mm;
    int precipitation_probability_pct;
    float wind_max_mps;
};

struct WeatherSnapshot {
    bool configured;
    bool has_data;
    bool in_progress;
    bool request_pending;
    uint32_t last_attempt_ms;
    uint32_t last_success_ms;
    uint32_t next_refresh_ms;
    time_t last_updated_epoch;
    time_t expires_epoch;
    char last_modified[64];
    char status[128];
    WeatherCurrent current;
    WeatherHourForecast hourly[WEATHER_MAX_HOURLY_POINTS];
    size_t hourly_count;
    WeatherDayForecast daily[WEATHER_MAX_DAILY_POINTS];
    size_t daily_count;
};

using WeatherWorkerWakeCallback = void (*)();

void weather_service_begin();
void weather_service_set_worker_wake_callback(WeatherWorkerWakeCallback callback);

bool weather_service_configured();
bool weather_service_has_data();
const char *weather_service_status();
uint32_t weather_service_last_attempt_ms();

bool weather_service_request_refresh(bool force, const char *reason);
bool weather_service_should_refresh(uint32_t now_ms);
bool weather_service_worker_has_pending();
bool weather_service_worker_busy();
bool weather_service_worker_fetch();
bool weather_service_take_publish_pending();

void weather_service_get_snapshot(WeatherSnapshot &out);
bool weather_service_get_day_forecast(time_t local_time, WeatherDayForecast &out);

const char *weather_condition_label(WeatherCondition condition);

float weather_display_temperature(float temperature_c);
float weather_display_wind(float wind_mps);
float weather_display_precip(float precip_mm);
const char *weather_temperature_unit();
const char *weather_wind_unit();
const char *weather_precip_unit();
