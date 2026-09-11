#include "weather_service.h"

#include "app_config.h"
#include "app_secrets.h"
#include "network_service.h"
#include "time_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace {

struct WeatherUnits {
    char temperature[8];
    char wind[16];
    char precip[8];
    char pressure[12];
};

struct DayAccumulator {
    bool valid;
    time_t local_day_epoch;
    float high_c;
    float low_c;
    float precipitation_mm;
    int precipitation_probability_pct;
    float wind_max_mps;
    WeatherCondition condition;
    int condition_rank;
    bool preferred_daytime_symbol;
    char symbol_code[40];
};

WeatherWorkerWakeCallback g_wake_callback = nullptr;
portMUX_TYPE g_weather_mux = portMUX_INITIALIZER_UNLOCKED;

WeatherCurrent g_current = {};
WeatherHourForecast g_hourly[WEATHER_MAX_HOURLY_POINTS] = {};
WeatherDayForecast g_daily[WEATHER_MAX_DAILY_POINTS] = {};
size_t g_hourly_count = 0;
size_t g_daily_count = 0;

bool g_has_data = false;
bool g_request_pending = false;
bool g_in_progress = false;
bool g_publish_pending = false;

uint32_t g_last_attempt_ms = 0;
uint32_t g_last_success_ms = 0;
uint32_t g_next_refresh_ms = 0;
time_t g_last_updated_epoch = 0;
time_t g_expires_epoch = 0;
char g_last_modified[64] = {};
char g_status[128] = "Weather idle";

bool usable_value(const char *value, const char *placeholder) {
    if (!value || !value[0]) return false;
    if (placeholder && strcmp(value, placeholder) == 0) return false;
    return true;
}

String clean_base_url() {
    String base(HA_BASE_URL);
    while (base.endsWith("/")) base.remove(base.length() - 1);
    return base;
}

String url_encode(const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    String out;
    if (!value) return out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

void set_statusf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    portENTER_CRITICAL(&g_weather_mux);
    snprintf(g_status, sizeof(g_status), "%s", buffer);
    portEXIT_CRITICAL(&g_weather_mux);
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_iso8601(const char *text, time_t &epoch) {
    epoch = 0;
    if (!text || strlen(text) < 10) return false;

    int year = 0;
    int month = 0;
    int day = 0;
    if (sscanf(text, "%d-%d-%d", &year, &month, &day) != 3) return false;

    int hour = 0;
    int minute = 0;
    int second = 0;
    if (strlen(text) >= 19) {
        char separator = 0;
        if (sscanf(text, "%d-%d-%d%c%d:%d:%d", &year, &month, &day,
                   &separator, &hour, &minute, &second) != 7 ||
            (separator != 'T' && separator != ' ')) {
            return false;
        }
    }

    const char *zone = text + ((strlen(text) >= 19) ? 19 : 10);
    while (*zone && *zone != 'Z' && *zone != 'z' && *zone != '+' && *zone != '-') ++zone;

    if (*zone == 'Z' || *zone == 'z' || *zone == '+' || *zone == '-') {
        int offset_seconds = 0;
        if (*zone == '+' || *zone == '-') {
            const int sign = (*zone == '+') ? 1 : -1;
            int zh = 0;
            int zm = 0;
            if (sscanf(zone + 1, "%d:%d", &zh, &zm) < 1) return false;
            offset_seconds = sign * (zh * 3600 + zm * 60);
        }
        const int64_t seconds = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL +
                                hour * 3600LL + minute * 60LL + second - offset_seconds;
        epoch = static_cast<time_t>(seconds);
        return true;
    }

    struct tm local_tm = {};
    local_tm.tm_year = year - 1900;
    local_tm.tm_mon = month - 1;
    local_tm.tm_mday = day;
    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = second;
    local_tm.tm_isdst = -1;
    epoch = mktime(&local_tm);
    return epoch > 0;
}

WeatherCondition map_ha_condition(const char *condition) {
    if (!condition || !condition[0]) return WeatherCondition::Unknown;
    if (strcmp(condition, "sunny") == 0 || strcmp(condition, "clear-night") == 0) {
        return WeatherCondition::Clear;
    }
    if (strcmp(condition, "partlycloudy") == 0 || strcmp(condition, "windy-variant") == 0) {
        return WeatherCondition::PartlyCloudy;
    }
    if (strcmp(condition, "cloudy") == 0 || strcmp(condition, "windy") == 0) {
        return WeatherCondition::Cloudy;
    }
    if (strcmp(condition, "fog") == 0) return WeatherCondition::Fog;
    if (strcmp(condition, "lightning") == 0 || strcmp(condition, "lightning-rainy") == 0) {
        return WeatherCondition::Thunderstorm;
    }
    if (strcmp(condition, "snowy") == 0 || strcmp(condition, "snowy-rainy") == 0 || strcmp(condition, "hail") == 0) {
        return WeatherCondition::Snow;
    }
    if (strcmp(condition, "rainy") == 0 || strcmp(condition, "pouring") == 0) {
        return WeatherCondition::Rain;
    }
    if (strcmp(condition, "exceptional") == 0) return WeatherCondition::Showers;
    return WeatherCondition::Unknown;
}

int condition_rank(WeatherCondition condition) {
    switch (condition) {
        case WeatherCondition::Thunderstorm: return 90;
        case WeatherCondition::Snow: return 80;
        case WeatherCondition::Rain: return 70;
        case WeatherCondition::Showers: return 65;
        case WeatherCondition::Fog: return 55;
        case WeatherCondition::Cloudy: return 45;
        case WeatherCondition::PartlyCloudy: return 35;
        case WeatherCondition::Clear: return 25;
        case WeatherCondition::Unknown: return 0;
    }
    return 0;
}

bool same_local_day(time_t left, time_t right) {
    struct tm left_tm = {};
    struct tm right_tm = {};
    localtime_r(&left, &left_tm);
    localtime_r(&right, &right_tm);
    return left_tm.tm_year == right_tm.tm_year && left_tm.tm_yday == right_tm.tm_yday;
}

float temperature_to_c(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (unit[0] == 'F' || unit[0] == 'f') return (value - 32.0f) * 5.0f / 9.0f;
    return value;
}

float wind_to_mps(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (strcasecmp(unit, "mph") == 0) return value * 0.44704f;
    if (strcasecmp(unit, "km/h") == 0 || strcasecmp(unit, "kph") == 0) return value / 3.6f;
    if (strcasecmp(unit, "kn") == 0 || strcasecmp(unit, "knot") == 0 || strcasecmp(unit, "knots") == 0) {
        return value * 0.514444f;
    }
    return value;
}

float precip_to_mm(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (strcasecmp(unit, "in") == 0 || strcasecmp(unit, "inch") == 0 || strcasecmp(unit, "inches") == 0) {
        return value * 25.4f;
    }
    if (strcasecmp(unit, "cm") == 0) return value * 10.0f;
    return value;
}

float pressure_to_hpa(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (strcasecmp(unit, "hPa") == 0 || strcasecmp(unit, "mbar") == 0) return value;
    if (strcasecmp(unit, "Pa") == 0) return value / 100.0f;
    if (strcasecmp(unit, "inHg") == 0) return value * 33.8639f;
    if (strcasecmp(unit, "psi") == 0) return value * 68.9476f;
    return value;
}

bool parse_forecast_array(JsonDocument &doc, JsonArrayConst &forecast) {
    forecast = doc["service_response"][HA_WEATHER_ENTITY]["forecast"].as<JsonArrayConst>();
    if (!forecast.isNull()) return true;

    forecast = doc[HA_WEATHER_ENTITY]["forecast"].as<JsonArrayConst>();
    if (!forecast.isNull()) return true;

    forecast = doc["forecast"].as<JsonArrayConst>();
    return !forecast.isNull();
}

int http_request(const char *method, const String &path, const String *body, String &payload) {
    payload = "";
    const String url = clean_base_url() + path;

    HTTPClient http;
    http.setConnectTimeout(WEATHER_HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);
    http.setUserAgent(String("FamilyCalendar/") + APP_VERSION);

    const String auth = String("Bearer ") + HA_ACCESS_TOKEN;
    int code = -1;

    auto execute = [&](auto &client) -> int {
        if (!http.begin(client, url)) return -21;
        http.addHeader("Authorization", auth);
        http.addHeader("Accept", "application/json");
        if (body) http.addHeader("Content-Type", "application/json");

        int result = -1;
        if (strcmp(method, "GET") == 0) {
            result = http.GET();
        } else if (strcmp(method, "POST") == 0 && body) {
            result = http.POST(*body);
        }

        if (result > 0) payload = http.getString();
        http.end();
        return result;
    };

    if (url.startsWith("https://")) {
        WiFiClientSecure client;
#if defined(HA_TLS_CA_CERT_PEM)
        client.setCACert(HA_TLS_CA_CERT_PEM);
#elif HA_TLS_ALLOW_INSECURE
        client.setInsecure();
#else
        return -20;
#endif
#if defined(HA_TLS_CA_CERT_PEM) || HA_TLS_ALLOW_INSECURE
        code = execute(client);
#endif
    } else {
        WiFiClient client;
        code = execute(client);
    }

    return code;
}

int http_get(const String &path, String &payload) {
    return http_request("GET", path, nullptr, payload);
}

int http_post(const String &path, const String &body, String &payload) {
    return http_request("POST", path, &body, payload);
}

bool fetch_current_state(WeatherCurrent &current, WeatherUnits &units, time_t &updated_epoch) {
    String payload;
    const String path = String("/api/states/") + url_encode(HA_WEATHER_ENTITY);
    const int code = http_get(path, payload);
    if (code != 200) {
        ESP_LOGW("FamilyCalendar", "[Weather] HA current-state HTTP %d", code);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.is<JsonObject>()) {
        ESP_LOGW("FamilyCalendar", "[Weather] Current-state JSON parse failed: %s",
                 error ? error.c_str() : "not object");
        return false;
    }

    JsonObjectConst attrs = doc["attributes"].as<JsonObjectConst>();
    const char *condition = doc["state"] | "";
    const char *updated = doc["last_updated"] | "";

    memset(&current, 0, sizeof(current));
    current.valid = true;
    current.condition = map_ha_condition(condition);
    snprintf(current.symbol_code, sizeof(current.symbol_code), "%s", condition);
    current.humidity_pct = attrs["humidity"].isNull() ? -1 : attrs["humidity"].as<int>();

    snprintf(units.temperature, sizeof(units.temperature), "%s", attrs["temperature_unit"] | "C");
    snprintf(units.wind, sizeof(units.wind), "%s", attrs["wind_speed_unit"] | "m/s");
    snprintf(units.precip, sizeof(units.precip), "%s", attrs["precipitation_unit"] | "mm");
    snprintf(units.pressure, sizeof(units.pressure), "%s", attrs["pressure_unit"] | "hPa");

    if (!attrs["temperature"].isNull()) {
        current.temperature_c = temperature_to_c(attrs["temperature"].as<float>(), units.temperature);
    }
    if (!attrs["wind_speed"].isNull()) {
        current.wind_mps = wind_to_mps(attrs["wind_speed"].as<float>(), units.wind);
    }
    if (!attrs["wind_bearing"].isNull()) {
        current.wind_direction_deg = attrs["wind_bearing"].as<float>();
    } else {
        current.wind_direction_deg = NAN;
    }
    if (!attrs["pressure"].isNull()) {
        current.pressure_hpa = pressure_to_hpa(attrs["pressure"].as<float>(), units.pressure);
    } else {
        current.pressure_hpa = NAN;
    }
    if (!attrs["precipitation"].isNull()) {
        current.precipitation_mm = precip_to_mm(attrs["precipitation"].as<float>(), units.precip);
    }
    current.precipitation_probability_pct = attrs["precipitation_probability"].isNull()
                                                ? -1
                                                : attrs["precipitation_probability"].as<int>();

    if (!parse_iso8601(updated, updated_epoch)) {
        updated_epoch = time_service_now();
    }
    current.epoch = updated_epoch;

    return true;
}

bool fetch_daily_forecast(const WeatherUnits &units,
                          WeatherDayForecast *daily,
                          size_t &daily_count,
                          const time_t now_epoch) {
    JsonDocument request;
    request["entity_id"] = HA_WEATHER_ENTITY;
    request["type"] = "daily";
    String body;
    serializeJson(request, body);

    String payload;
    const int code = http_post("/api/services/weather/get_forecasts?return_response", body, payload);
    if (code != 200) {
        ESP_LOGW("FamilyCalendar", "[Weather] HA daily forecast HTTP %d", code);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        ESP_LOGW("FamilyCalendar", "[Weather] Daily forecast JSON parse failed: %s", error.c_str());
        return false;
    }

    JsonArrayConst forecast;
    if (!parse_forecast_array(doc, forecast)) {
        ESP_LOGW("FamilyCalendar", "[Weather] Daily forecast array missing");
        return false;
    }

    memset(daily, 0, WEATHER_MAX_DAILY_POINTS * sizeof(WeatherDayForecast));
    daily_count = 0;

    for (JsonObjectConst point : forecast) {
        if (daily_count >= WEATHER_MAX_DAILY_POINTS) break;

        const char *datetime = point["datetime"] | point["time"] | "";
        time_t epoch = 0;
        if (!parse_iso8601(datetime, epoch)) continue;
        if (epoch + 86400 < now_epoch) continue;

        WeatherDayForecast &out = daily[daily_count++];
        out.valid = true;
        out.local_day_epoch = epoch;

        const char *condition = point["condition"] | "";
        out.condition = map_ha_condition(condition);
        snprintf(out.symbol_code, sizeof(out.symbol_code), "%s", condition);

        float high = NAN;
        if (!point["temperature"].isNull()) high = point["temperature"].as<float>();
        if (isnan(high) && !point["temperature_max"].isNull()) high = point["temperature_max"].as<float>();
        if (isnan(high) && !point["temp"].isNull()) high = point["temp"].as<float>();

        float low = NAN;
        if (!point["templow"].isNull()) low = point["templow"].as<float>();
        if (isnan(low) && !point["temperature_min"].isNull()) low = point["temperature_min"].as<float>();

        out.high_c = isnan(high) ? NAN : temperature_to_c(high, units.temperature);
        out.low_c = isnan(low) ? out.high_c : temperature_to_c(low, units.temperature);

        out.precipitation_mm = point["precipitation"].isNull()
                                   ? 0.0f
                                   : precip_to_mm(point["precipitation"].as<float>(), units.precip);
        out.precipitation_probability_pct = point["precipitation_probability"].isNull()
                                                ? -1
                                                : static_cast<int>(lroundf(point["precipitation_probability"].as<float>()));
        out.wind_max_mps = point["wind_speed"].isNull()
                               ? 0.0f
                               : wind_to_mps(point["wind_speed"].as<float>(), units.wind);

        struct tm local_tm = {};
        localtime_r(&out.local_day_epoch, &local_tm);
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        local_tm.tm_isdst = -1;
        out.local_day_epoch = mktime(&local_tm);
    }

    return daily_count > 0;
}

bool fetch_hourly_forecast(const WeatherUnits &units,
                           WeatherHourForecast *hourly,
                           size_t &hourly_count,
                           WeatherDayForecast *daily_fallback,
                           size_t &daily_fallback_count,
                           const time_t now_epoch,
                           WeatherCurrent &current) {
    JsonDocument request;
    request["entity_id"] = HA_WEATHER_ENTITY;
    request["type"] = "hourly";
    String body;
    serializeJson(request, body);

    String payload;
    const int code = http_post("/api/services/weather/get_forecasts?return_response", body, payload);
    if (code != 200) {
        ESP_LOGW("FamilyCalendar", "[Weather] HA hourly forecast HTTP %d", code);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        ESP_LOGW("FamilyCalendar", "[Weather] Hourly forecast JSON parse failed: %s", error.c_str());
        return false;
    }

    JsonArrayConst forecast;
    if (!parse_forecast_array(doc, forecast)) {
        ESP_LOGW("FamilyCalendar", "[Weather] Hourly forecast array missing");
        return false;
    }

    memset(hourly, 0, WEATHER_MAX_HOURLY_POINTS * sizeof(WeatherHourForecast));
    memset(daily_fallback, 0, WEATHER_MAX_DAILY_POINTS * sizeof(WeatherDayForecast));
    hourly_count = 0;
    daily_fallback_count = 0;

    DayAccumulator accum[WEATHER_MAX_DAILY_POINTS] = {};
    bool current_temp_backfilled = !isnan(current.temperature_c);

    size_t parsed = 0;
    for (JsonObjectConst point : forecast) {
        const char *datetime = point["datetime"] | point["time"] | "";
        time_t epoch = 0;
        if (!parse_iso8601(datetime, epoch)) continue;

        const char *condition_text = point["condition"] | "";
        const WeatherCondition condition = map_ha_condition(condition_text);

        const float temp_raw = point["temperature"].isNull() ? NAN : point["temperature"].as<float>();
        const float temp_c = isnan(temp_raw) ? NAN : temperature_to_c(temp_raw, units.temperature);
        const float wind_raw = point["wind_speed"].isNull() ? 0.0f : point["wind_speed"].as<float>();
        const float wind_mps = wind_to_mps(wind_raw, units.wind);
        const float precip_raw = point["precipitation"].isNull() ? 0.0f : point["precipitation"].as<float>();
        const float precip_mm = precip_to_mm(precip_raw, units.precip);
        const int precip_prob = point["precipitation_probability"].isNull()
                                    ? -1
                                    : static_cast<int>(lroundf(point["precipitation_probability"].as<float>()));

        if (epoch >= now_epoch && hourly_count < WEATHER_MAX_HOURLY_POINTS) {
            WeatherHourForecast &out = hourly[hourly_count++];
            out.valid = true;
            out.epoch = epoch;
            out.condition = condition;
            snprintf(out.symbol_code, sizeof(out.symbol_code), "%s", condition_text);
            out.temperature_c = temp_c;
            out.precipitation_mm = precip_mm;
            out.precipitation_probability_pct = precip_prob;
            out.wind_mps = wind_mps;

            if (!current_temp_backfilled && !isnan(temp_c)) {
                current.temperature_c = temp_c;
                current.condition = condition;
                snprintf(current.symbol_code, sizeof(current.symbol_code), "%s", condition_text);
                current_temp_backfilled = true;
            }
        }

        struct tm local_tm = {};
        localtime_r(&epoch, &local_tm);
        const int local_hour = local_tm.tm_hour;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        local_tm.tm_isdst = -1;
        const time_t day_epoch = mktime(&local_tm);

        int slot = -1;
        for (size_t i = 0; i < WEATHER_MAX_DAILY_POINTS; ++i) {
            if (accum[i].valid && accum[i].local_day_epoch == day_epoch) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot < 0) {
            for (size_t i = 0; i < WEATHER_MAX_DAILY_POINTS; ++i) {
                if (!accum[i].valid) {
                    slot = static_cast<int>(i);
                    accum[i].valid = true;
                    accum[i].local_day_epoch = day_epoch;
                    accum[i].high_c = isnan(temp_c) ? -1000.0f : temp_c;
                    accum[i].low_c = isnan(temp_c) ? 1000.0f : temp_c;
                    accum[i].precipitation_mm = 0.0f;
                    accum[i].precipitation_probability_pct = -1;
                    accum[i].wind_max_mps = 0.0f;
                    accum[i].condition = condition;
                    accum[i].condition_rank = condition_rank(condition);
                    accum[i].preferred_daytime_symbol = false;
                    snprintf(accum[i].symbol_code, sizeof(accum[i].symbol_code), "%s", condition_text);
                    break;
                }
            }
        }

        if (slot >= 0) {
            DayAccumulator &day = accum[slot];
            if (!isnan(temp_c)) {
                if (temp_c > day.high_c) day.high_c = temp_c;
                if (temp_c < day.low_c) day.low_c = temp_c;
            }
            day.precipitation_mm += precip_mm;
            if (precip_prob > day.precipitation_probability_pct) {
                day.precipitation_probability_pct = precip_prob;
            }
            if (wind_mps > day.wind_max_mps) day.wind_max_mps = wind_mps;

            const int rank = condition_rank(condition);
            const bool daytime = local_hour >= 11 && local_hour <= 16;
            if (daytime && !day.preferred_daytime_symbol && rank > 0) {
                day.condition = condition;
                snprintf(day.symbol_code, sizeof(day.symbol_code), "%s", condition_text);
                day.condition_rank = rank;
                day.preferred_daytime_symbol = true;
            } else if (!day.preferred_daytime_symbol && rank >= day.condition_rank) {
                day.condition = condition;
                snprintf(day.symbol_code, sizeof(day.symbol_code), "%s", condition_text);
                day.condition_rank = rank;
            }
        }

        ++parsed;
        if ((parsed % 12U) == 0U) vTaskDelay(1);
    }

    for (size_t i = 0; i < WEATHER_MAX_DAILY_POINTS; ++i) {
        if (!accum[i].valid) continue;
        WeatherDayForecast &out = daily_fallback[daily_fallback_count++];
        out.valid = true;
        out.local_day_epoch = accum[i].local_day_epoch;
        out.condition = accum[i].condition;
        snprintf(out.symbol_code, sizeof(out.symbol_code), "%s", accum[i].symbol_code);
        out.high_c = accum[i].high_c < -900.0f ? NAN : accum[i].high_c;
        out.low_c = accum[i].low_c > 900.0f ? out.high_c : accum[i].low_c;
        out.precipitation_mm = accum[i].precipitation_mm;
        out.precipitation_probability_pct = accum[i].precipitation_probability_pct;
        out.wind_max_mps = accum[i].wind_max_mps;
    }

    return hourly_count > 0;
}

void log_next_refresh(uint32_t now_ms, uint32_t next_refresh_ms) {
    uint32_t remaining_ms = 0;
    if (static_cast<int32_t>(next_refresh_ms - now_ms) > 0) {
        remaining_ms = next_refresh_ms - now_ms;
    }
    const unsigned long minutes = remaining_ms / 60000UL;
    const unsigned long seconds = (remaining_ms % 60000UL) / 1000UL;
    ESP_LOGI("FamilyCalendar", "[Weather] Next refresh in %lum %lus", minutes, seconds);
}

} // namespace

void weather_service_begin() {
    if (!weather_service_configured()) {
        set_statusf("Weather not configured");
        return;
    }
    set_statusf("Weather ready (Home Assistant)");
}

void weather_service_set_worker_wake_callback(WeatherWorkerWakeCallback callback) {
    g_wake_callback = callback;
}

bool weather_service_configured() {
    return usable_value(HA_BASE_URL, nullptr) &&
           usable_value(HA_ACCESS_TOKEN, "YOUR_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN") &&
           usable_value(HA_WEATHER_ENTITY, "");
}

bool weather_service_has_data() {
    bool has_data = false;
    portENTER_CRITICAL(&g_weather_mux);
    has_data = g_has_data;
    portEXIT_CRITICAL(&g_weather_mux);
    return has_data;
}

const char *weather_service_status() {
    return g_status;
}

uint32_t weather_service_last_attempt_ms() {
    uint32_t last_attempt = 0;
    portENTER_CRITICAL(&g_weather_mux);
    last_attempt = g_last_attempt_ms;
    portEXIT_CRITICAL(&g_weather_mux);
    return last_attempt;
}

bool weather_service_should_refresh(uint32_t now_ms) {
    if (!weather_service_configured()) return false;

    bool has_data = false;
    bool in_progress = false;
    uint32_t next_refresh_ms = 0;

    portENTER_CRITICAL(&g_weather_mux);
    has_data = g_has_data;
    in_progress = g_in_progress;
    next_refresh_ms = g_next_refresh_ms;
    portEXIT_CRITICAL(&g_weather_mux);

    if (in_progress) return false;
    if (!has_data) return true;
    if (next_refresh_ms == 0) return true;
    return static_cast<int32_t>(now_ms - next_refresh_ms) >= 0;
}

bool weather_service_request_refresh(bool force, const char *reason) {
    if (!weather_service_configured()) {
        set_statusf("Weather not configured");
        return false;
    }

    const uint32_t now_ms = millis();
    if (!force && !weather_service_should_refresh(now_ms)) {
        ESP_LOGI("FamilyCalendar", "[Weather] Cache fresh; network request skipped (%s)",
                 reason ? reason : "no reason");
        return false;
    }

    bool queued = false;
    portENTER_CRITICAL(&g_weather_mux);
    if (!g_request_pending && !g_in_progress) {
        g_request_pending = true;
        queued = true;
        snprintf(g_status, sizeof(g_status), "Weather refresh queued");
    }
    portEXIT_CRITICAL(&g_weather_mux);

    if (!queued) return false;

    ESP_LOGI("FamilyCalendar", "[Weather] Fetch queued from Home Assistant (%s)",
             reason ? reason : "no reason");
    if (g_wake_callback) g_wake_callback();
    return true;
}

bool weather_service_worker_has_pending() {
    bool pending = false;
    portENTER_CRITICAL(&g_weather_mux);
    pending = g_request_pending;
    portEXIT_CRITICAL(&g_weather_mux);
    return pending;
}

bool weather_service_worker_busy() {
    bool busy = false;
    portENTER_CRITICAL(&g_weather_mux);
    busy = g_request_pending || g_in_progress;
    portEXIT_CRITICAL(&g_weather_mux);
    return busy;
}

bool weather_service_worker_fetch() {
    if (!weather_service_configured()) {
        portENTER_CRITICAL(&g_weather_mux);
        g_request_pending = false;
        portEXIT_CRITICAL(&g_weather_mux);
        return false;
    }

    if (!network_service_connected()) return false;

    const uint32_t now_ms = millis();
    if (!weather_service_should_refresh(now_ms)) {
        portENTER_CRITICAL(&g_weather_mux);
        g_request_pending = false;
        snprintf(g_status, sizeof(g_status), "Weather cache still fresh");
        portEXIT_CRITICAL(&g_weather_mux);
        ESP_LOGI("FamilyCalendar", "[Weather] Cache fresh; network request skipped");
        return false;
    }

    bool should_fetch = false;
    portENTER_CRITICAL(&g_weather_mux);
    if (g_request_pending && !g_in_progress) {
        should_fetch = true;
        g_request_pending = false;
        g_in_progress = true;
        g_last_attempt_ms = now_ms;
        snprintf(g_status, sizeof(g_status), "Weather fetch in progress");
    }
    portEXIT_CRITICAL(&g_weather_mux);
    if (!should_fetch) return false;

    ESP_LOGI("FamilyCalendar", "[Weather] Fetch start (source: Home Assistant)");

    WeatherCurrent current = {};
    WeatherUnits units = {};
    WeatherHourForecast hourly[WEATHER_MAX_HOURLY_POINTS] = {};
    WeatherDayForecast daily[WEATHER_MAX_DAILY_POINTS] = {};
    WeatherDayForecast daily_from_hourly[WEATHER_MAX_DAILY_POINTS] = {};
    size_t hourly_count = 0;
    size_t daily_count = 0;
    size_t daily_from_hourly_count = 0;
    time_t updated_epoch = 0;

    const uint32_t started_ms = millis();
    const time_t now_epoch = time_service_now();

    const bool current_ok = fetch_current_state(current, units, updated_epoch);
    if (current_ok) vTaskDelay(pdMS_TO_TICKS(HA_HTTP_INTER_REQUEST_GAP_MS));

    const bool daily_ok = fetch_daily_forecast(units, daily, daily_count, now_epoch);
    if (daily_ok) vTaskDelay(pdMS_TO_TICKS(HA_HTTP_INTER_REQUEST_GAP_MS));

    const bool hourly_ok = fetch_hourly_forecast(units,
                                                 hourly,
                                                 hourly_count,
                                                 daily_from_hourly,
                                                 daily_from_hourly_count,
                                                 now_epoch,
                                                 current);

    const uint32_t elapsed_ms = millis() - started_ms;

    if (!current_ok || (!daily_ok && !hourly_ok)) {
        ESP_LOGW("FamilyCalendar", "[Weather] HA weather fetch failed in %lums", static_cast<unsigned long>(elapsed_ms));
        const uint32_t retry_due_ms = now_ms + WEATHER_RETRY_INTERVAL_MS;
        portENTER_CRITICAL(&g_weather_mux);
        g_next_refresh_ms = retry_due_ms;
        g_in_progress = false;
        snprintf(g_status, sizeof(g_status), "Weather fetch failed");
        portEXIT_CRITICAL(&g_weather_mux);
        log_next_refresh(now_ms, retry_due_ms);
        return true;
    }

    if (!daily_ok && hourly_ok) {
        memcpy(daily, daily_from_hourly, sizeof(daily));
        daily_count = daily_from_hourly_count;
    }

    if (daily_count > 0) {
        size_t idx = 0;
        bool found_today = false;
        for (size_t i = 0; i < daily_count; ++i) {
            if (same_local_day(daily[i].local_day_epoch, now_epoch)) {
                idx = i;
                found_today = true;
                break;
            }
        }
        if (!found_today) idx = 0;
        current.today_high_c = daily[idx].high_c;
        current.today_low_c = daily[idx].low_c;
    } else {
        current.today_high_c = current.temperature_c;
        current.today_low_c = current.temperature_c;
    }

    const uint32_t next_refresh_ms = now_ms + WEATHER_ACTIVE_TAB_REFRESH_MS;

    portENTER_CRITICAL(&g_weather_mux);
    g_current = current;
    memcpy(g_hourly, hourly, sizeof(g_hourly));
    memcpy(g_daily, daily, sizeof(g_daily));
    g_hourly_count = hourly_count;
    g_daily_count = daily_count;
    g_last_updated_epoch = updated_epoch;
    g_expires_epoch = 0;
    g_last_modified[0] = '\0';
    g_last_success_ms = now_ms;
    g_next_refresh_ms = next_refresh_ms;
    g_has_data = true;
    g_publish_pending = true;
    g_in_progress = false;
    snprintf(g_status, sizeof(g_status), "Weather updated from Home Assistant");
    portEXIT_CRITICAL(&g_weather_mux);

    ESP_LOGI("FamilyCalendar", "[Weather] HTTP 200 (HA APIs) in %lums", static_cast<unsigned long>(elapsed_ms));
    ESP_LOGI("FamilyCalendar", "[Weather] Parsed %u hourly / %u daily entries",
             static_cast<unsigned>(hourly_count), static_cast<unsigned>(daily_count));
    log_next_refresh(now_ms, next_refresh_ms);
    return true;
}

bool weather_service_take_publish_pending() {
    bool changed = false;
    portENTER_CRITICAL(&g_weather_mux);
    changed = g_publish_pending;
    g_publish_pending = false;
    portEXIT_CRITICAL(&g_weather_mux);
    return changed;
}

void weather_service_get_snapshot(WeatherSnapshot &out) {
    memset(&out, 0, sizeof(out));
    portENTER_CRITICAL(&g_weather_mux);
    out.configured = weather_service_configured();
    out.has_data = g_has_data;
    out.in_progress = g_in_progress;
    out.request_pending = g_request_pending;
    out.last_attempt_ms = g_last_attempt_ms;
    out.last_success_ms = g_last_success_ms;
    out.next_refresh_ms = g_next_refresh_ms;
    out.last_updated_epoch = g_last_updated_epoch;
    out.expires_epoch = g_expires_epoch;
    snprintf(out.last_modified, sizeof(out.last_modified), "%s", g_last_modified);
    snprintf(out.status, sizeof(out.status), "%s", g_status);
    out.current = g_current;
    memcpy(out.hourly, g_hourly, sizeof(out.hourly));
    memcpy(out.daily, g_daily, sizeof(out.daily));
    out.hourly_count = g_hourly_count;
    out.daily_count = g_daily_count;
    portEXIT_CRITICAL(&g_weather_mux);
}

bool weather_service_get_day_forecast(time_t local_time, WeatherDayForecast &out) {
    memset(&out, 0, sizeof(out));
    WeatherDayForecast daily_copy[WEATHER_MAX_DAILY_POINTS] = {};
    size_t daily_count = 0;

    portENTER_CRITICAL(&g_weather_mux);
    daily_count = g_daily_count;
    if (daily_count > WEATHER_MAX_DAILY_POINTS) daily_count = WEATHER_MAX_DAILY_POINTS;
    memcpy(daily_copy, g_daily, daily_count * sizeof(WeatherDayForecast));
    portEXIT_CRITICAL(&g_weather_mux);

    for (size_t i = 0; i < daily_count; ++i) {
        if (!daily_copy[i].valid) continue;
        if (same_local_day(daily_copy[i].local_day_epoch, local_time)) {
            out = daily_copy[i];
            return true;
        }
    }
    return false;
}

const char *weather_condition_label(WeatherCondition condition) {
    switch (condition) {
        case WeatherCondition::Clear: return "Clear";
        case WeatherCondition::PartlyCloudy: return "Partly cloudy";
        case WeatherCondition::Cloudy: return "Cloudy";
        case WeatherCondition::Rain: return "Rain";
        case WeatherCondition::Showers: return "Showers";
        case WeatherCondition::Thunderstorm: return "Thunderstorms";
        case WeatherCondition::Snow: return "Snow";
        case WeatherCondition::Fog: return "Fog";
        case WeatherCondition::Unknown: return "Unknown";
    }
    return "Unknown";
}

float weather_display_temperature(float temperature_c) {
#if WEATHER_USE_IMPERIAL
    return temperature_c * 9.0f / 5.0f + 32.0f;
#else
    return temperature_c;
#endif
}

float weather_display_wind(float wind_mps) {
#if WEATHER_USE_IMPERIAL
    return wind_mps * 2.236936f;
#else
    return wind_mps;
#endif
}

float weather_display_precip(float precip_mm) {
#if WEATHER_USE_IMPERIAL
    return precip_mm / 25.4f;
#else
    return precip_mm;
#endif
}

const char *weather_temperature_unit() {
#if WEATHER_USE_IMPERIAL
    return "F";
#else
    return "C";
#endif
}

const char *weather_wind_unit() {
#if WEATHER_USE_IMPERIAL
    return "mph";
#else
    return "m/s";
#endif
}

const char *weather_precip_unit() {
#if WEATHER_USE_IMPERIAL
    return "in";
#else
    return "mm";
#endif
}
